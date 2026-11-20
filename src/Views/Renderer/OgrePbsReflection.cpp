#include "OgrePbsReflection.h"

#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorNodeDef.h>
#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/Pass/OgreCompositorPassDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <Cubemaps/OgreCubemapProbe.h>
#include <Cubemaps/OgreParallaxCorrectedCubemapBase.h>

#include <OgreCamera.h>
#include <OgreHlms.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsPbs.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreHlmsUnlitDatablock.h>
#include <OgreManualObject2.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreVisibilityFlags.h>
#include <Vao/OgreStagingBuffer.h>
#include <Vao/OgreConstBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace View3D {
namespace {

// 天空球只给探针相机看：视口的 view bit 从最低位往上分配，取 RESERVED 掩码
// （0x3fffffff，bit0-29）里最靠上的 bit29。bit30/31 不在相机掩码内，会被剔除，
// 实测那样整份反射退化成清屏色。
constexpr std::uint32_t kSkyVisibilityBit = 1u << 29u;
constexpr float kSkyRadius = 1.0e4f;
constexpr std::uint32_t kProbeResolution = 128u;
constexpr float kProbeExtent = 5.0e4f;
constexpr std::uint8_t kSkyRenderQueue = 10u;
// CubemapProbe 的 const buffer 布局：6 个 float4。
constexpr std::size_t kProbeConstBufferSize = 6u * 4u * sizeof(float);

Ogre::ColourValue skyColour(float normalisedHeight)
{
    const Ogre::ColourValue ground(0.030f, 0.034f, 0.040f, 1.0f);
    const Ogre::ColourValue horizon(0.260f, 0.290f, 0.330f, 1.0f);
    // 上方亮、下方暗：朝上的金属面（框、顶盖）出白，面罩朝前的部分保持深色。
    const Ogre::ColourValue zenith(0.470f, 0.510f, 0.570f, 1.0f);
    const auto blend = [](const Ogre::ColourValue& a, const Ogre::ColourValue& b, float t) {
        return Ogre::ColourValue(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                                 a.b + (b.b - a.b) * t, 1.0f);
    };
    if (normalisedHeight <= 0.0f)
        return blend(ground, horizon, std::clamp(normalisedHeight + 1.0f, 0.0f, 1.0f));
    return blend(horizon, zenith, std::clamp(normalisedHeight, 0.0f, 1.0f));
}

// 环境光分布：整体偏暗，只有一个大块柔光（摄影棚灯箱的效果）。金属暗面
// 保持深色、亮面才有高光；均匀的亮天空会把金属整体洗成一片灰。
Ogre::ColourValue skyColourDirection(const Ogre::Vector3& direction)
{
    const Ogre::ColourValue base = skyColour(direction.y);
    const Ogre::Vector3 softbox = Ogre::Vector3(0.30f, 0.72f, 0.62f).normalisedCopy();
    const float sunDot = std::max(0.0f, direction.dotProduct(softbox));
    const float broad = std::pow(sunDot, 3.0f) * 1.35f;
    const float core = std::pow(sunDot, 22.0f) * 1.10f;
    const float amount = broad + core;
    return Ogre::ColourValue(std::min(1.0f, base.r + amount),
                             std::min(1.0f, base.g + amount),
                             std::min(1.0f, base.b + amount * 0.90f), 1.0f);
}

} // namespace

// 最小探针系统：只实现手动探针需要的那几个接口，不创建官方的混合/拷贝工作区。
class PbsReflection::System final : public Ogre::ParallaxCorrectedCubemapBase {
  public:
    System(Ogre::Root* root, Ogre::SceneManager* sceneManager,
           const Ogre::CompositorWorkspaceDef* workspaceDef)
        : ParallaxCorrectedCubemapBase(
              Ogre::Id::generateNewId<Ogre::ParallaxCorrectedCubemapBase>(), root, sceneManager,
              workspaceDef, /*automaticMode=*/false)
    {
    }

    ~System() override
    {
        if (m_staging) {
            m_staging->removeReferenceCount();
            m_staging = nullptr;
        }
    }

    void updateAllDirtyProbes() override
    {
        for (Ogre::CubemapProbe* probe : getProbes()) {
            if (!probe || !probe->mEnabled || !probe->mDirty)
                continue;
            probe->_prepareForRendering();
            probe->_updateRender();
            probe->mDirty = false;
        }
    }

    void _addManuallyActiveProbe(Ogre::CubemapProbe* probe) override
    {
        if (std::find(m_manual.begin(), m_manual.end(), probe) == m_manual.end())
            m_manual.push_back(probe);
    }

    void _removeManuallyActiveProbe(Ogre::CubemapProbe* probe) override
    {
        m_manual.erase(std::remove(m_manual.begin(), m_manual.end(), probe), m_manual.end());
    }

    size_t getConstBufferSize() override { return kProbeConstBufferSize; }

    void fillConstBufferData(const Ogre::Matrix4& viewMatrix,
                             float* passBufferPtr) const override
    {
        // 手动探针模式下着色器读的是每材质的 manualProbe，pass buffer 里的
        // autoProbe 不会被采样；写零保持布局一致即可。
        std::memset(passBufferPtr, 0, kProbeConstBufferSize);
    }

    void _notifyPreparePassHash(const Ogre::Matrix4& viewMatrix) override
    {
        if (m_manual.empty() || viewMatrix == m_cachedView)
            return;
        Ogre::Matrix3 invViewMat3;
        viewMatrix.extract3x3Matrix(invViewMat3);
        invViewMat3 = invViewMat3.Inverse();

        const size_t needed = m_manual.size() * kProbeConstBufferSize;
        Ogre::VaoManager* vaoManager =
            getSceneManager()->getDestinationRenderSystem()->getVaoManager();
        if (!m_staging || m_staging->getMaxSize() < needed) {
            if (m_staging)
                m_staging->removeReferenceCount();
            m_staging = vaoManager->getStagingBuffer(std::max<size_t>(needed, 512u), true);
        }
        Ogre::StagingBuffer::DestinationVec destinations;
        float* data = static_cast<float*>(m_staging->map(needed));
        size_t offset = 0;
        for (Ogre::CubemapProbe* probe : m_manual) {
            Ogre::ConstBufferPacked* buffer = probe->getConstBufferForManualProbes();
            if (!buffer)
                continue;
            destinations.push_back(
                Ogre::StagingBuffer::Destination(buffer, 0, offset, kProbeConstBufferSize));
            Ogre::ParallaxCorrectedCubemapBase::fillConstBufferData(*probe, viewMatrix, invViewMat3,
                                                                    data);
            data += kProbeConstBufferSize >> 2u;
            offset += kProbeConstBufferSize;
        }
        m_staging->unmap(destinations);
        m_cachedView = viewMatrix;
    }

  private:
    std::vector<Ogre::CubemapProbe*> m_manual;
    Ogre::StagingBuffer* m_staging = nullptr;
    Ogre::Matrix4 m_cachedView = Ogre::Matrix4::ZERO;
};

PbsReflection::PbsReflection(Ogre::Root* root, Ogre::SceneManager* sceneManager)
    : m_root(root), m_sceneManager(sceneManager)
{
    const std::string suffix = std::to_string(Ogre::Id::generateNewId<Ogre::HlmsUnlitDatablock>());
    m_nodeName = "View3DPbsProbeNode" + suffix;
    m_workspaceName = "View3DPbsProbeWorkspace" + suffix;
    m_skyDatablockName = "View3DPbsSky" + suffix;
}

PbsReflection::~PbsReflection()
{
    shutdown();
}

bool PbsReflection::createSkyDome()
{
    Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
    if (!unlit)
        return false;
    Ogre::HlmsMacroblock macroblock;
    macroblock.mDepthCheck = false;
    macroblock.mDepthWrite = false;
    macroblock.mCullMode = Ogre::CULL_NONE;
    Ogre::HlmsBlendblock blendblock;
    blendblock.setBlendType(Ogre::SBT_REPLACE);
    m_skyDatablock = static_cast<Ogre::HlmsUnlitDatablock*>(unlit->createDatablock(
        m_skyDatablockName, m_skyDatablockName, macroblock, blendblock, Ogre::HlmsParamVec()));
    if (!m_skyDatablock)
        return false;
    m_skyDatablock->setUseColour(true);

    m_sky = m_sceneManager->createManualObject();
    m_sky->begin(m_skyDatablockName, Ogre::OT_TRIANGLE_LIST);
    const int rings = 16;
    const int segments = 32;
    for (int ring = 0; ring <= rings; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(rings);
        const float phi = v * Ogre::Math::PI;
        const float y = std::cos(phi);
        const float r = std::sin(phi);
        for (int segment = 0; segment <= segments; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float theta = u * Ogre::Math::TWO_PI;
            const float px = r * std::cos(theta);
            const float pz = r * std::sin(theta);
            m_sky->position(kSkyRadius * px, kSkyRadius * y, kSkyRadius * pz);
            m_sky->colour(skyColourDirection(Ogre::Vector3(px, y, pz)));
        }
    }
    for (int ring = 0; ring < rings; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            const unsigned a = static_cast<unsigned>(ring * (segments + 1) + segment);
            const unsigned b = a + 1u;
            const unsigned c = a + static_cast<unsigned>(segments + 1);
            const unsigned d = c + 1u;
            m_sky->triangle(a, b, c);
            m_sky->triangle(b, d, c);
        }
    }
    m_sky->end();
    m_sky->setCastShadows(false);
    m_sky->setVisibilityFlags(kSkyVisibilityBit);
    m_sky->setRenderQueueGroup(kSkyRenderQueue);
    m_skyNode = m_sceneManager->getRootSceneNode(Ogre::SCENE_DYNAMIC)
                    ->createChildSceneNode(Ogre::SCENE_DYNAMIC);
    m_skyNode->attachObject(m_sky);
    return true;
}

bool PbsReflection::createProbeDefinitions()
{
    Ogre::CompositorManager2* compositor = m_root->getCompositorManager2();
    if (!compositor)
        return false;
    if (!compositor->hasNodeDefinition(Ogre::IdString(m_nodeName))) {
        Ogre::CompositorNodeDef* node = compositor->addNodeDefinition(m_nodeName);
        node->addTextureSourceName("cubemap", 0, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
        // initWorkspace 固定传两个通道，第二个留作 IBL 输出（本实现只渲染颜色）。
        node->addTextureSourceName("ibl", 1, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
        node->setNumTargetPass(6);
        for (std::uint32_t face = 0; face < 6u; ++face) {
            Ogre::CompositorTargetDef* target = node->addTargetPass("cubemap", face);
            target->setNumPasses(1);
            auto* pass = static_cast<Ogre::CompositorPassSceneDef*>(
                target->addPass(Ogre::PASS_SCENE));
            pass->mCameraCubemapReorient = true;
            pass->mVisibilityMask = kSkyVisibilityBit;
            pass->mFirstRQ = 0u;
            pass->mLastRQ = kSkyRenderQueue + 1u;
            pass->mEnableForwardPlus = false;
            pass->mIncludeOverlays = false;
            pass->mLoadActionColour[0] = Ogre::LoadAction::Clear;
            pass->mClearColour[0] = skyColour(0.0f);
            pass->mLoadActionDepth = Ogre::LoadAction::Clear;
            pass->mLoadActionStencil = Ogre::LoadAction::Clear;
        }
    }
    if (!compositor->hasWorkspaceDefinition(Ogre::IdString(m_workspaceName))) {
        Ogre::CompositorWorkspaceDef* def = compositor->addWorkspaceDefinition(m_workspaceName);
        def->connectExternal(0, Ogre::IdString(m_nodeName), 0);
        def->connectExternal(1, Ogre::IdString(m_nodeName), 1);
    }
    return true;
}

bool PbsReflection::createProbe()
{
    Ogre::CompositorManager2* compositor = m_root->getCompositorManager2();
    const Ogre::CompositorWorkspaceDef* def =
        compositor->getWorkspaceDefinitionNoThrow(Ogre::IdString(m_workspaceName));
    if (!def)
        return false;
    m_system = std::make_unique<System>(m_root, m_sceneManager, def);
    m_probe = m_system->createProbe();
    if (!m_probe)
        return false;
    // useManual=false：探针纹理本身就是渲染目标（1 级 mip），datablock 直接采样它。
    m_probe->setTextureParams(kProbeResolution, kProbeResolution, false,
                              Ogre::PFG_RGBA8_UNORM_SRGB, true);
    m_probe->initWorkspace(1.0f, kSkyRadius * 2.0f);
    const Ogre::Aabb shape(Ogre::Vector3::ZERO, Ogre::Vector3(kProbeExtent));
    m_probe->set(Ogre::Vector3::ZERO, shape, Ogre::Vector3::UNIT_SCALE, Ogre::Matrix3::IDENTITY,
                 shape);
    // 不在这里立刻渲染：此时天空球的顶点缓冲还没建好，探针只会采到清屏色
    // （实测整份反射退化成一块纯色）。留到第 2 帧由 frameStarted 渲染一次。
    m_probe->mDirty = true;

    if (auto* pbs = dynamic_cast<Ogre::HlmsPbs*>(
            m_root->getHlmsManager()->getHlms(Ogre::HLMS_PBS)))
        pbs->setParallaxCorrectedCubemap(m_system.get());
    return true;
}

bool PbsReflection::initialize()
{
    if (!m_root || !m_sceneManager)
        return false;
    if (m_probe)
        return true;
    try {
        if (!createSkyDome() || !createProbeDefinitions() || !createProbe()) {
            shutdown();
            return false;
        }
    } catch (const Ogre::Exception& error) {
        std::cerr << "VIEW3D_PBS_REFLECTION_FAILED reason=" << error.getFullDescription()
                  << std::endl;
        shutdown();
        return false;
    }
    m_root->addFrameListener(this);
    std::cerr << "VIEW3D_PBS_REFLECTION_OK probe=" << kProbeResolution << " skybit=0x"
              << std::hex << kSkyVisibilityBit << " reserved=0x"
              << Ogre::VisibilityFlags::RESERVED_VISIBILITY_FLAGS << std::dec << std::endl;
    return true;
}

void PbsReflection::bindDatablock(Ogre::HlmsPbsDatablock* datablock) const
{
    if (datablock && m_probe)
        datablock->setCubemapProbe(m_probe);
}

bool PbsReflection::frameStarted(const Ogre::FrameEvent& event)
{
    static_cast<void>(event);
    ++m_frameTicks;
    if (m_system && m_probe && m_probe->mDirty && m_frameTicks >= 2)
        m_system->updateAllDirtyProbes();
    return true;
}

void PbsReflection::shutdown()
{
    if (!m_root)
        return;
    m_root->removeFrameListener(this);
    if (m_system) {
        if (auto* pbs = dynamic_cast<Ogre::HlmsPbs*>(
                m_root->getHlmsManager()->getHlms(Ogre::HLMS_PBS)))
            pbs->setParallaxCorrectedCubemap(nullptr);
        if (m_probe) {
            m_system->destroyProbe(m_probe);
            m_probe = nullptr;
        }
        m_system.reset();
    }
    if (m_skyNode) {
        m_skyNode->detachAllObjects();
        m_sceneManager->destroySceneNode(m_skyNode);
        m_skyNode = nullptr;
    }
    if (m_sky) {
        m_sceneManager->destroyManualObject(m_sky);
        m_sky = nullptr;
    }
    if (m_skyDatablock) {
        if (Ogre::Hlms* unlit = m_root->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT))
            unlit->destroyDatablock(m_skyDatablockName);
        m_skyDatablock = nullptr;
    }
    if (Ogre::CompositorManager2* compositor = m_root->getCompositorManager2()) {
        if (compositor->hasWorkspaceDefinition(Ogre::IdString(m_workspaceName)))
            compositor->removeWorkspaceDefinition(Ogre::IdString(m_workspaceName));
        if (compositor->hasNodeDefinition(Ogre::IdString(m_nodeName)))
            compositor->removeNodeDefinition(Ogre::IdString(m_nodeName));
    }
}
} // namespace View3D

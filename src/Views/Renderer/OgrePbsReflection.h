#pragma once

#include <OgreFrameListener.h>
#include <OgreMatrix3.h>
#include <OgreMatrix4.h>
#include <OgrePrerequisites.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Ogre {
class CubemapProbe;
class HlmsPbsDatablock;
class HlmsUnlitDatablock;
class ManualObject;
class SceneNode;
class StagingBuffer;
class ParallaxCorrectedCubemapBase;
}

namespace View3D {

// PBS 环境反射：给金属/玻璃材质提供一张"天空环境"cubemap。
// Ogre-Next 3 的 PBS 反射只走 CubemapProbe（PBSM_REFLECTION + 每材质 const
// buffer），没有裸 cubemap 槽位；而官方的 ParallaxCorrectedCubemap 会拖进一套
// 探针混合/拷贝材质（本工程运行时解析不了）。这里直接派生子类：
// 天空球只对探针可见（最高位 visibility flag，视口永不使用），静态渲染一次，
// 探针以手动模式挂到每个 PBS datablock，pass const buffer 自己上传。
// 开关 VIEW3D_PBS_REFLECTION=1，未开启时零资源。
class PbsReflection final : public Ogre::FrameListener {
  public:
    PbsReflection(Ogre::Root* root, Ogre::SceneManager* sceneManager);
    ~PbsReflection() override;

    bool initialize();
    void shutdown();
    bool ready() const noexcept { return m_probe != nullptr; }

    void bindDatablock(Ogre::HlmsPbsDatablock* datablock) const;

    bool frameStarted(const Ogre::FrameEvent& event) override;

  private:
    class System;
    bool createSkyDome();
    bool createProbeDefinitions();
    bool createProbe();

    Ogre::Root* m_root = nullptr;
    Ogre::SceneManager* m_sceneManager = nullptr;
    Ogre::ManualObject* m_sky = nullptr;
    Ogre::SceneNode* m_skyNode = nullptr;
    Ogre::HlmsUnlitDatablock* m_skyDatablock = nullptr;
    std::unique_ptr<System> m_system;
    Ogre::CubemapProbe* m_probe = nullptr;
    std::string m_nodeName;
    std::string m_workspaceName;
    std::string m_skyDatablockName;
    std::uint32_t m_frameTicks = 0;
};
}

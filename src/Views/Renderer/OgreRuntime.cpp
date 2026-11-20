#include "OgreRuntime.h"

#include <OgreAbiUtils.h>
#include <OgreArchiveManager.h>
#include <OgreHlms.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsPbs.h>
#include <OgreHlmsUnlit.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>

#include <QDir>
#include <QMetaObject>
#include <QObject>
#include <QProcessEnvironment>
#include <QThread>

#include <atomic>
#include <iostream>

namespace View3D {

namespace {

void registerUnlitHlms(const QString& runtimeDirectory, Ogre::Root* root)
{
    Ogre::String mainFolder;
    Ogre::StringVector libraryFolders;
    Ogre::HlmsUnlit::getDefaultPaths(mainFolder, libraryFolders);
    const QString configuredRoot =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("VIEW3D_HLMS_ROOT"));
    QString selectedRoot = configuredRoot.isEmpty() ? runtimeDirectory : configuredRoot;
    if (configuredRoot.isEmpty() &&
        QDir(runtimeDirectory).exists(QStringLiteral("Hlms/Common")))
        selectedRoot = QStringLiteral(".");
    const Ogre::String hlmsRoot = QDir(selectedRoot).path().toStdString() + "/";
    Ogre::ArchiveManager& archives = Ogre::ArchiveManager::getSingleton();
    Ogre::Archive* mainArchive =
        archives.load(hlmsRoot + mainFolder, "FileSystem", true);
    Ogre::ArchiveVec libraries;
    for (const Ogre::String& folder : libraryFolders)
        libraries.push_back(archives.load(hlmsRoot + folder, "FileSystem", true));
    auto* unlit = OGRE_NEW Ogre::HlmsUnlit(mainArchive, &libraries);
    root->getHlmsManager()->registerHlms(unlit);
    const Ogre::String materialName("View3DUnlit");
    unlit->createDatablock(materialName, materialName, Ogre::HlmsMacroblock(),
                           Ogre::HlmsBlendblock(), Ogre::HlmsParamVec());
}

void registerPbsHlms(const QString& runtimeDirectory, Ogre::Root* root)
{
    Ogre::String mainFolder;
    Ogre::StringVector libraryFolders;
    Ogre::HlmsPbs::getDefaultPaths(mainFolder, libraryFolders);
    const QString configuredRoot =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("VIEW3D_HLMS_ROOT"));
    QString selectedRoot = configuredRoot.isEmpty() ? runtimeDirectory : configuredRoot;
    if (configuredRoot.isEmpty() &&
        QDir(runtimeDirectory).exists(QStringLiteral("Hlms/Common")))
        selectedRoot = QStringLiteral(".");
    const Ogre::String hlmsRoot = QDir(selectedRoot).path().toStdString() + "/";
    Ogre::ArchiveManager& archives = Ogre::ArchiveManager::getSingleton();
    Ogre::Archive* mainArchive = archives.load(hlmsRoot + mainFolder, "FileSystem", true);
    Ogre::ArchiveVec libraries;
    for (const Ogre::String& folder : libraryFolders)
        libraries.push_back(archives.load(hlmsRoot + folder, "FileSystem", true));
    auto* pbs = OGRE_NEW Ogre::HlmsPbs(mainArchive, &libraries);
    root->getHlmsManager()->registerHlms(pbs);
}

} // namespace

struct OgreRuntime::State {
    std::unique_ptr<Ogre::Root> root;
    bool hlmsReady = false;
};

OgreRuntime::OgreRuntime() : m_state(std::make_unique<State>()) {}

OgreRuntime::~OgreRuntime()
{
    shutdown();
}

bool OgreRuntime::vsyncEnabled()
{
    const QString value = qEnvironmentVariable("VIEW3D_VSYNC");
    if (value.isEmpty())
        return false;
    return value == QLatin1String("1") || value.compare(QLatin1String("yes"),
                                                         Qt::CaseInsensitive) == 0 ||
        value.compare(QLatin1String("on"), Qt::CaseInsensitive) == 0;
}

bool OgreRuntime::initialize()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_gate);
    if (!m_state || m_state->root)
        return m_state && m_state->root != nullptr;
    try {
        const Ogre::AbiCookie cookie = Ogre::generateAbiCookie();
        auto root =
            std::make_unique<Ogre::Root>(&cookie, "", "", "View3D.Ogre.log", "View3D");
        root->loadPlugin("RenderSystem_Direct3D11.dll", false, nullptr);
        Ogre::RenderSystem* renderer =
            root->getRenderSystemByName("Direct3D11 Rendering Subsystem");
        if (!renderer) {
            std::cerr << "VIEW3D_OGRE_ROOT_FAILED: Direct3D11 not available\n";
            return false;
        }
        renderer->setConfigOption("Rendering Device", "(default)");
        renderer->setConfigOption("Full Screen", "No");
        renderer->setConfigOption("VSync", vsyncEnabled() ? "Yes" : "No");
        root->setRenderSystem(renderer);
        root->initialise(false);
        m_state->root = std::move(root);
        std::cerr << "VIEW3D_OGRE_STAGE: shared_root_ready\n";
        return true;
    } catch (const Ogre::Exception& error) {
        std::cerr << "VIEW3D_OGRE_ROOT_FAILED: " << error.getFullDescription() << "\n";
        return false;
    }
}

bool OgreRuntime::ensureHlms(const QString& runtimeDirectory)
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_gate);
    if (m_state->hlmsReady)
        return true;
    if (!m_state->root)
        return false;
    try {
        registerUnlitHlms(runtimeDirectory, m_state->root.get());
        registerPbsHlms(runtimeDirectory, m_state->root.get());
        m_state->hlmsReady = true;
        std::cerr << "VIEW3D_OGRE_STAGE: shared_hlms_ready\n";
        return true;
    } catch (const Ogre::Exception& error) {
        std::cerr << "VIEW3D_OGRE_HLMS_FAILED: " << error.getFullDescription() << "\n";
        return false;
    }
}

void OgreRuntime::shutdown()
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_gate);
    if (!m_state)
        return;
    m_state->hlmsReady = false;
    if (m_state->root) {
        m_state->root.reset();
        std::cerr << "VIEW3D_OGRE_STAGE: shared_root_shutdown\n";
    }
}

bool OgreRuntime::ready() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_gate);
    return m_state && m_state->root != nullptr;
}

bool OgreRuntime::hlmsReady() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_gate);
    return m_state && m_state->hlmsReady;
}

Ogre::Root* OgreRuntime::root() const noexcept
{
    std::lock_guard<std::recursive_mutex> ogreGuard(m_gate);
    return m_state ? m_state->root.get() : nullptr;
}

void OgreRuntime::postToUi(std::function<void()> callback) const
{
    if (!callback)
        return;
    if (!m_uiContext || QThread::currentThread() == m_uiContext->thread()) {
        callback();
        return;
    }
    QObject* context = m_uiContext;
    QMetaObject::invokeMethod(
        context, [callback = std::move(callback)] { callback(); }, Qt::QueuedConnection);
}

std::uint64_t nextOgreResourceSerial()
{
    static std::atomic<std::uint64_t> serial { 0 };
    return ++serial;
}

} // namespace View3D

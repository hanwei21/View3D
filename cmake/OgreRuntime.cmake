# Ogre-Next 运行期产物的位置：由 View3D 宿主统一部署，渲染层之外不再拼路径。
include_guard(GLOBAL)

if(NOT DEFINED OGRE_NEXT_ROOT)
    message(FATAL_ERROR
        "OGRE_NEXT_ROOT must point to the Ogre-Next v3.0.0 install prefix")
endif()

foreach(_ogre_bin Release release)
    if(EXISTS "${OGRE_NEXT_ROOT}/bin/${_ogre_bin}/OgreMain.dll")
        set(OGRE_RUNTIME_DIR "${OGRE_NEXT_ROOT}/bin/${_ogre_bin}")
        break()
    endif()
endforeach()

if(NOT DEFINED OGRE_RUNTIME_DIR)
    message(FATAL_ERROR "No OgreMain.dll under ${OGRE_NEXT_ROOT}/bin")
endif()

set(OGRE_RUNTIME_DLLS
    "${OGRE_RUNTIME_DIR}/OgreMain.dll"
    "${OGRE_RUNTIME_DIR}/OgreHlmsPbs.dll"
    "${OGRE_RUNTIME_DIR}/OgreHlmsUnlit.dll"
    "${OGRE_RUNTIME_DIR}/RenderSystem_Direct3D11.dll"
    "${OGRE_RUNTIME_DIR}/brotlicommon.dll"
    "${OGRE_RUNTIME_DIR}/brotlidec.dll"
    "${OGRE_RUNTIME_DIR}/bz2.dll"
    "${OGRE_RUNTIME_DIR}/freetype.dll"
    "${OGRE_RUNTIME_DIR}/libpng16.dll"
    "${OGRE_RUNTIME_DIR}/z.dll"
    "${OGRE_RUNTIME_DIR}/zzip.dll")

set(OGRE_HLMS_MEDIA_DIRS
    "${OGRE_NEXT_ROOT}/Media/Hlms/Common"
    "${OGRE_NEXT_ROOT}/Media/Hlms/Unlit"
    "${OGRE_NEXT_ROOT}/Media/Hlms/Pbs")

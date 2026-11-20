#pragma once

#if defined(_MSC_VER)
#  if defined(PLUGIN_ENGINE_BUILD)
#    define PLUGIN_ENGINE_EXPORT __declspec(dllexport)
#  else
#    define PLUGIN_ENGINE_EXPORT __declspec(dllimport)
#  endif
#else
#  define PLUGIN_ENGINE_EXPORT
#endif

#pragma once
#include <PluginFactoryRegistry.h>
#define VIEW3D_DEFINE_PLUGIN_ENTRY(plugin_id_literal, register_body)                          \
    extern "C" __declspec(dllexport) const char *view3dPluginId() {                           \
        return plugin_id_literal;                                                                  \
    }                                                                                              \
    extern "C" __declspec(dllexport) void registerPluginFactories(                                 \
        RF::PluginEngine::PluginFactoryRegistry *registry) {                                       \
        if (registry) {                                                                            \
            register_body                                                                          \
        }                                                                                          \
    }

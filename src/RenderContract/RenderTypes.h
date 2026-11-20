#pragma once

#include <array>
#include <cstdint>

namespace View3D {

struct CaptureOptions {
    bool includeGrid = false;
    bool includeAxes = false;
    bool includeMeasurementOverlay = false;
};

enum class ViewportDisplayMode { Solid, Wireframe, SolidWireframe, Points };
enum class LayerRenderState { Unknown, Pending, Ready, Failed };
enum class ViewportBackgroundMode { Solid, Gradient };
enum class ViewportShadowQuality { Performance, Balanced, Quality };
enum class ViewportShadowFilter { Pcf2x2, Pcf3x3, Pcf4x4 };

struct ViewportBackgroundStyle {
    ViewportBackgroundMode mode = ViewportBackgroundMode::Gradient;
    float topRed = 0.082f;
    float topGreen = 0.125f;
    float topBlue = 0.2f;
    float bottomRed = 0.031f;
    float bottomGreen = 0.051f;
    float bottomBlue = 0.086f;
};

struct ViewportLightingStyle {
    bool ambientVisible = true;
    float ambientRed = 1.0f;
    float ambientGreen = 1.0f;
    float ambientBlue = 1.0f;
    float ambientIntensity = 0.35f;
    bool mainLightVisible = true;
    float mainLightRed = 1.0f;
    float mainLightGreen = 0.957f;
    float mainLightBlue = 0.878f;
    float mainLightIntensity = 1.5f;
    float mainLightDirectionX = -0.45f;
    float mainLightDirectionY = -0.80f;
    float mainLightDirectionZ = -0.35f;
};

struct ViewportShadowStyle {
    bool enabled = true;
    ViewportShadowQuality quality = ViewportShadowQuality::Balanced;

    bool operator==(const ViewportShadowStyle& other) const noexcept
    {
        return enabled == other.enabled && quality == other.quality;
    }
};

struct ViewportEnvironmentStyle {
    bool gridVisible = true;
    bool axesVisible = true;
    float gridRed = 0.30f;
    float gridGreen = 0.34f;
    float gridBlue = 0.42f;
    float gridExtent = 20.0f;
    std::uint32_t gridDivisions = 40;
    float axisLength = 5.0f;
};

struct FeatureHighlightStyle {
    float red = 1.0f;
    float green = 0.78f;
    float blue = 0.05f;
    float opacity = 0.78f;
    float depthBias = -1.0f;
};

enum class PbrTextureColourSpace { None, Linear, Srgb };
enum class PbrTangentSource { None, Explicit, Generated };

struct PbrMaterialState {
    float metallic = 1.0f;
    float roughness = 1.0f;
    std::array<float, 3> emissive {};
    float normalScale = 1.0f;
    bool diffuseTexture = false;
    bool metallicTexture = false;
    bool roughnessTexture = false;
    bool normalTexture = false;
    bool occlusionTexture = false;
    bool emissiveTexture = false;
    std::uint8_t diffuseUv = 0;
    std::uint8_t metallicUv = 0;
    std::uint8_t roughnessUv = 0;
    std::uint8_t normalUv = 0;
    std::uint8_t occlusionUv = 0;
    std::uint8_t emissiveUv = 0;
    std::uint8_t metallicDerivedSample = 0;
    std::uint8_t roughnessDerivedSample = 0;
    std::uint8_t occlusionDerivedSample = 0;
    PbrTextureColourSpace diffuseColourSpace = PbrTextureColourSpace::None;
    PbrTextureColourSpace metallicColourSpace = PbrTextureColourSpace::None;
    PbrTextureColourSpace roughnessColourSpace = PbrTextureColourSpace::None;
    PbrTextureColourSpace normalColourSpace = PbrTextureColourSpace::None;
    PbrTextureColourSpace occlusionColourSpace = PbrTextureColourSpace::None;
    PbrTextureColourSpace emissiveColourSpace = PbrTextureColourSpace::None;
    PbrTangentSource tangentSource = PbrTangentSource::None;
    bool aoApproximation = false;

    bool operator==(const PbrMaterialState& other) const noexcept
    {
        return metallic == other.metallic && roughness == other.roughness &&
            emissive == other.emissive && normalScale == other.normalScale &&
            diffuseTexture == other.diffuseTexture &&
            metallicTexture == other.metallicTexture &&
            roughnessTexture == other.roughnessTexture &&
            normalTexture == other.normalTexture &&
            occlusionTexture == other.occlusionTexture &&
            emissiveTexture == other.emissiveTexture &&
            diffuseUv == other.diffuseUv && metallicUv == other.metallicUv &&
            roughnessUv == other.roughnessUv && normalUv == other.normalUv &&
            occlusionUv == other.occlusionUv && emissiveUv == other.emissiveUv &&
            metallicDerivedSample == other.metallicDerivedSample &&
            roughnessDerivedSample == other.roughnessDerivedSample &&
            occlusionDerivedSample == other.occlusionDerivedSample &&
            diffuseColourSpace == other.diffuseColourSpace &&
            metallicColourSpace == other.metallicColourSpace &&
            roughnessColourSpace == other.roughnessColourSpace &&
            normalColourSpace == other.normalColourSpace &&
            occlusionColourSpace == other.occlusionColourSpace &&
            emissiveColourSpace == other.emissiveColourSpace &&
            tangentSource == other.tangentSource && aoApproximation == other.aoApproximation;
    }
};

struct ViewportSettings {
    ViewportDisplayMode displayMode = ViewportDisplayMode::Solid;
    ViewportBackgroundStyle background;
    ViewportLightingStyle lighting;
    ViewportShadowStyle shadows;
    ViewportEnvironmentStyle environment;
    std::uint64_t renderBudgetMaxVertices = 50000000u;
    double targetFrameRate = 60.0;
    // 二维（水平面）模式：进入时锁定正俯视、上北下南，不接受旋转。
    bool planarMode = false;
    // 球面（WGS84 椭球）模式：只对 3D Tiles 数据生效，与二维模式互斥。
    bool globeMode = false;
};

} // namespace View3D

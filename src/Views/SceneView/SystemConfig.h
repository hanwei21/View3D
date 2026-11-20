#pragma once

#include <RenderTypes.h>
#include <TileCacheSettings.h>

#include <QString>

namespace View3D {

struct ViewportCaptureConfig {
    QString defaultFormat = QStringLiteral("png");
    int jpegQuality = 90;
    double outputScale = 1.0;
    bool includeWidgetOverlays = false;
};

struct SystemConfig {
    ViewportCaptureConfig viewportCapture;
    ViewportSettings viewport;
    TileCacheSettings tileCache;

    static SystemConfig load(const QString& filePath, QString* warning = nullptr);
};

} // namespace View3D

#pragma once

#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QUuid>

namespace View3D {

using LayerId = QUuid;

enum class LayerType {
    Unknown = 0,
    Group,
    Model,
    Tiled,
    Imagery,
    Feature
};

enum class LoadState {
    Unloaded = 0,
    Queued,
    Loading,
    PartiallyReady,
    Ready,
    Failed,
    Cancelling,
    Cancelled,
    Offline,
    SourceInvalid
};

struct ErrorInfo {
    QString code;
    QString message;
    QString source;
    QString stage;
    QString technicalDetails;
    QString suggestedAction;
    bool retryable { false };
};

} // namespace View3D

Q_DECLARE_METATYPE(View3D::LayerType)
Q_DECLARE_METATYPE(View3D::LoadState)
Q_DECLARE_METATYPE(View3D::ErrorInfo)

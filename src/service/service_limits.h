#ifndef AOWIS_HYDROLOGY_SERVICE_LIMITS_H
#define AOWIS_HYDROLOGY_SERVICE_LIMITS_H

// Internal limits shared by the service translation units. Not installed.
#include <QtGlobal>

namespace Aowis::Hydrology::Internal
{
// Rainfall scenario manifests embed the client's GeoJSON geometry. HTTP
// bodies are limited to 256 KiB, but the manifest adds metadata and Qt's
// compact re-serialization; the read limit must comfortably exceed the
// largest accepted request so every committed scenario stays readable.
// Creation rejects any manifest above this limit BEFORE committing it.
inline constexpr qint64 MaximumScenarioManifestBytes = 1024 * 1024;
} // namespace Aowis::Hydrology::Internal

#endif

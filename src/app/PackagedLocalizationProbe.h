#pragma once

#include <QString>

namespace PackagedLocalizationProbe
{

// Verifies the localization resources compiled into the shipping application
// and writes deterministic JSON evidence for the package/release pipeline.
bool run(const QString &reportPath, QString *error = nullptr);

} // namespace PackagedLocalizationProbe

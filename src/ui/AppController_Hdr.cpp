// AppController: HDR display status.
#include "ui/AppController.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "localization/NativeText.h"

#include <utility>

void AppController::refreshHdrStatus()
{
    const bool experimentalHdrEnabled =
        m_config->value(ConfigKeys::InternalCaptureExperimentalHdr,
                        ConfigKeys::InternalCaptureExperimentalHdrDefault).toBool();
    const capture::HdrReport refreshed =
        capture::HdrCapabilities::query(experimentalHdrEnabled);
    const bool displayChanged =
        m_hdrProbed && !capture::HdrCapabilities::sameDisplayState(m_hdr, refreshed);
    m_hdr = refreshed;
    m_hdrProbed = true;
    capture::HdrCapabilities::logReport(m_hdr);
    emit hdrStatusChanged();
    if (displayChanged)
        emit hdrDisplayConfigurationChanged();
}

void AppController::pollHdrStatus()
{
    if (!m_hdrProbed)
        return;

    const bool experimentalHdrEnabled =
        m_config->value(ConfigKeys::InternalCaptureExperimentalHdr,
                        ConfigKeys::InternalCaptureExperimentalHdrDefault).toBool();
    capture::HdrReport refreshed =
        capture::HdrCapabilities::refreshDisplayState(m_hdr, experimentalHdrEnabled);
    if (capture::HdrCapabilities::sameDisplayState(m_hdr, refreshed))
        return;

    m_hdr = std::move(refreshed);
    capture::HdrCapabilities::logReport(m_hdr);
    emit hdrStatusChanged();
    emit hdrDisplayConfigurationChanged();
}

bool AppController::hdrDisplayActive() const
{
    return m_hdr.anyHdrActive;
}

QString AppController::hdrStatusText() const
{
    if (!m_hdrProbed) {
        //: HDR status before GameHQ has queried the graphics driver.
        //% "Not checked yet"
        return NativeText::get(QT_TRID_NOOP("gamehq.hdr.status.not_checked"),
                               "Not checked yet");
    }
    if (m_hdr.outputs.isEmpty()) {
        //: HDR status when the graphics driver reports no displays.
        //% "No displays reported by the graphics driver"
        return NativeText::get(QT_TRID_NOOP("gamehq.hdr.status.no_displays"),
                               "No displays reported by the graphics driver");
    }
    if (m_hdr.anyHdrActive) {
        //: HDR status when Windows HDR is active on at least one display.
        //% "Windows HDR is active"
        return NativeText::get(QT_TRID_NOOP("gamehq.hdr.status.active"),
                               "Windows HDR is active");
    }
    //: HDR status when Windows HDR is inactive on every display.
    //% "Windows HDR is inactive"
    return NativeText::get(QT_TRID_NOOP("gamehq.hdr.status.inactive"),
                           "Windows HDR is inactive");
}

QString AppController::hdrDetailText() const
{
    if (!m_hdrProbed)
        //: HDR detail shown before the first graphics-driver query.
        //% "Check the current HDR state of every display."
        return NativeText::get(QT_TRID_NOOP("gamehq.hdr.detail.not_checked"),
                               "Check the current HDR state of every display.");
    return m_hdr.summaryLines().join(QLatin1Char('\n'));
}

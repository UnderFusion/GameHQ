// InputEngine: the HidHide "fix hidden controller" flow (elevated whitelist
// helper launch and exit-code watch). Split out of InputEngine.cpp; the class
// itself is declared in InputEngine.h.
#include "input/InputEngine.h"

#include "input/DualSenseDevice.h"
#include "input/HidCloakMonitor.h"
#include "localization/NativeText.h"

#include <QDebug>
#include <QTimer>

#include <windows.h>

void InputEngine::fixHiddenController()
{
    if (m_fixProcess)
        return;   // helper already in flight

    void* process = HidCloakMonitor::launchElevatedWhitelistHelper();
    if (!process) {
        setControllerWarning(
            //: Hidden-controller fix result when the UAC prompt is declined.
            //% "Administrator approval was declined — the controller stays hidden. You can whitelist GameHQ manually in the HidHide Configuration Client."
            NativeText::get(QT_TRID_NOOP("gamehq.input.hidden.fix_declined"),
                            "Administrator approval was declined — the controller stays "
                            "hidden. You can whitelist GameHQ manually in the HidHide "
                            "Configuration Client."),
            true);
        return;
    }
    m_fixProcess = process;
    //: Hidden-controller fix in progress while the UAC prompt is shown.
    //% "Applying the fix (administrator prompt)..."
    setControllerWarning(NativeText::get(QT_TRID_NOOP("gamehq.input.hidden.fix_applying"),
                                         "Applying the fix (administrator prompt)..."),
                         false);

    if (!m_fixWatch) {
        m_fixWatch = new QTimer(this);
        m_fixWatch->setInterval(500);
        connect(m_fixWatch, &QTimer::timeout, this, [this] {
            HANDLE h = static_cast<HANDLE>(m_fixProcess);
            if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0)
                return;   // still running
            DWORD code = 1;
            GetExitCodeProcess(h, &code);
            CloseHandle(h);
            m_fixProcess = nullptr;
            m_fixWatch->stop();
            if (code == 0) {
                qInfo() << "Input: GameHQ whitelisted in HidHide";
                setControllerWarning(
                    //: Hidden-controller fix succeeded.
                    //% "GameHQ is now whitelisted in HidHide. Unplug and replug the controller if it does not appear within a few seconds."
                    NativeText::get(QT_TRID_NOOP("gamehq.input.hidden.fix_done"),
                                    "GameHQ is now whitelisted in HidHide. Unplug and replug "
                                    "the controller if it does not appear within a few "
                                    "seconds."),
                    false);
                m_sonyPad->rescan();
            } else {
                qWarning() << "Input: HidHide whitelist helper failed, code" << code;
                setControllerWarning(
                    //: Hidden-controller fix failed; %1 is the helper exit code.
                    //% "Automatic whitelisting failed (code %1). Add GameHQ.exe on the Applications tab of the HidHide Configuration Client instead."
                    NativeText::get(QT_TRID_NOOP("gamehq.input.hidden.fix_failed"),
                                    "Automatic whitelisting failed (code %1). Add GameHQ.exe "
                                    "on the Applications tab of the HidHide Configuration "
                                    "Client instead.").arg(code),
                    true);
            }
        });
    }
    m_fixWatch->start();
}

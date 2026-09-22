#include "overlay/OverlayPresenter.h"

#include <QWindow>

#include <windows.h>

static_assert(OverlayWin32::kExNoActivate == WS_EX_NOACTIVATE, "WS_EX_NOACTIVATE drifted");
static_assert(OverlayWin32::kSwpNoSize == SWP_NOSIZE, "SWP_NOSIZE drifted");
static_assert(OverlayWin32::kSwpNoMove == SWP_NOMOVE, "SWP_NOMOVE drifted");
static_assert(OverlayWin32::kSwpNoZOrder == SWP_NOZORDER, "SWP_NOZORDER drifted");
static_assert(OverlayWin32::kSwpNoActivate == SWP_NOACTIVATE, "SWP_NOACTIVATE drifted");
static_assert(OverlayWin32::kSwpFrameChanged == SWP_FRAMECHANGED, "SWP_FRAMECHANGED drifted");

QString OverlayPresentReport::toLogString() const
{
    return QStringLiteral("hwnd=0x%1 recreated=%2 style_applied=%3 activatable=%7"
                          " foreground 0x%4 -> 0x%5 (%6)")
        .arg(QString::number(reinterpret_cast<qulonglong>(handle), 16))
        .arg(recreated ? 1 : 0)
        .arg(styleApplied ? 1 : 0)
        .arg(QString::number(reinterpret_cast<qulonglong>(foregroundBefore), 16))
        .arg(QString::number(reinterpret_cast<qulonglong>(foregroundAfter), 16))
        .arg(foregroundPreserved() ? QStringLiteral("preserved")
                                   : QStringLiteral("CHANGED"))
        .arg(activatable ? 1 : 0);
}

OverlayPresenter::OverlayPresenter(std::unique_ptr<OverlayWindowApi> api)
    : m_api(std::move(api))
{
}

OverlayPresenter::~OverlayPresenter() = default;

// Returns true when the ex-style had to be written. Called at every step that
// can hand back a different handle: Qt rebuilds the native window on some
// flag, geometry and screen transitions, and a rebuilt window starts without
// our ex-style — in EITHER mode, which is why the wanted state is recomputed
// here instead of being assumed from the last write.
bool OverlayPresenter::ensureActivationStyle(void* hwnd, OverlayPresentReport& report)
{
    if (!hwnd)
        return false;
    if (m_styledHandle && hwnd != m_styledHandle)
        report.recreated = true;

    const unsigned long exStyle = m_api->extendedStyle(hwnd);
    const bool hasNoActivate = (exStyle & OverlayWin32::kExNoActivate) != 0;
    const bool wantNoActivate = !m_activatable;
    if (hwnd == m_styledHandle && hasNoActivate == wantNoActivate)
        return false;

    m_api->setExtendedStyle(hwnd, wantNoActivate
                                      ? (exStyle | OverlayWin32::kExNoActivate)
                                      : (exStyle & ~OverlayWin32::kExNoActivate));
    // Frame-cache the style change without moving, resizing, reordering or
    // — above all — activating. Even when clearing WS_EX_NOACTIVATE this call
    // must not activate: the foreground move is a separate, explicit request.
    m_api->setWindowPos(hwnd, nullptr,
                        OverlayWin32::kSwpNoMove | OverlayWin32::kSwpNoSize
                            | OverlayWin32::kSwpNoZOrder | OverlayWin32::kSwpNoActivate
                            | OverlayWin32::kSwpFrameChanged);
    m_styledHandle = hwnd;
    report.styleApplied = true;
    return true;
}

OverlayPresentReport OverlayPresenter::present(const QRect& geometry)
{
    OverlayPresentReport report;
    report.activatable = m_activatable;
    report.foregroundBefore = m_api->foregroundWindow();

    // Style first: the window must carry the mode's ex-style BEFORE it can be
    // shown, moved or raised, not afterwards.
    ensureActivationStyle(m_api->nativeHandle(), report);

    if (geometry.isValid()) {
        m_api->setGeometry(geometry);
        ensureActivationStyle(m_api->nativeHandle(), report);
    }

    m_api->showWindow();

    void* hwnd = m_api->nativeHandle();
    ensureActivationStyle(hwnd, report);
    m_api->setWindowPos(hwnd, OverlayWin32::topmost(),
                        OverlayWin32::kSwpNoMove | OverlayWin32::kSwpNoSize
                            | OverlayWin32::kSwpNoActivate);

    report.handle = hwnd;
    report.foregroundAfter = m_api->foregroundWindow();
    return report;
}

OverlayPresentReport OverlayPresenter::reassert()
{
    OverlayPresentReport report;
    report.activatable = m_activatable;
    report.foregroundBefore = m_api->foregroundWindow();

    void* hwnd = m_api->nativeHandle();
    if (!hwnd) {
        report.foregroundAfter = report.foregroundBefore;
        return report;
    }
    ensureActivationStyle(hwnd, report);
    m_api->setWindowPos(hwnd, OverlayWin32::topmost(),
                        OverlayWin32::kSwpNoMove | OverlayWin32::kSwpNoSize
                            | OverlayWin32::kSwpNoActivate);

    report.handle = hwnd;
    report.foregroundAfter = m_api->foregroundWindow();
    return report;
}

// cpo-o06b. Deliberately narrow: only the ex-style changes. No geometry, no
// visibility, no z-order and no activation happen here — whether the overlay
// actually ends up in the foreground is decided by the explicit request the
// caller makes next (ForegroundAcquirer), and by Windows.
OverlayPresentReport OverlayPresenter::makeActivatable()
{
    m_activatable = true;

    OverlayPresentReport report;
    report.activatable = true;
    report.foregroundBefore = m_api->foregroundWindow();

    void* hwnd = m_api->nativeHandle();
    if (!hwnd) {
        report.foregroundAfter = report.foregroundBefore;
        return report;
    }
    ensureActivationStyle(hwnd, report);
    report.handle = hwnd;
    report.foregroundAfter = m_api->foregroundWindow();
    return report;
}

// --- production seam -------------------------------------------------------
namespace
{
class QWindowOverlayApi final : public OverlayWindowApi
{
public:
    explicit QWindowOverlayApi(QWindow* window)
        : m_window(window)
    {
    }

    void* nativeHandle() override
    {
        return m_window ? reinterpret_cast<void*>(m_window->winId()) : nullptr;
    }

    unsigned long extendedStyle(void* hwnd) override
    {
        return static_cast<unsigned long>(
            GetWindowLongPtrW(static_cast<HWND>(hwnd), GWL_EXSTYLE));
    }

    void setExtendedStyle(void* hwnd, unsigned long style) override
    {
        SetWindowLongPtrW(static_cast<HWND>(hwnd), GWL_EXSTYLE,
                          static_cast<LONG_PTR>(style));
    }

    void setWindowPos(void* hwnd, void* insertAfter, unsigned flags) override
    {
        ::SetWindowPos(static_cast<HWND>(hwnd), static_cast<HWND>(insertAfter),
                       0, 0, 0, 0, flags);
    }

    void setGeometry(const QRect& rect) override
    {
        if (m_window)
            m_window->setGeometry(rect);
    }

    void showWindow() override
    {
        if (m_window)
            m_window->show();
    }

    void* foregroundWindow() override { return GetForegroundWindow(); }

private:
    QWindow* m_window;
};
}  // namespace

std::unique_ptr<OverlayWindowApi> makeQWindowOverlayApi(QWindow* window)
{
    return std::make_unique<QWindowOverlayApi>(window);
}

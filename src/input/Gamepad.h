#pragma once
#include "input/ControlId.h"

#include <QObject>
#include <QString>

// Abstract gamepad backend (docs/controller-input.md). DualSense-first, but the
// interface lets XInput/SDL backends be added later. Buttons are logical, not
// device-specific; the concrete backend maps raw reports onto this enum.
class Gamepad : public QObject
{
    Q_OBJECT
public:
    enum Button {
        Share, Options, PS,
        Cross, Circle, Triangle, Square,
        L1, R1,
        DpadUp, DpadDown, DpadLeft, DpadRight,
        // Appended, not inserted: backends build a quint32 edge bitmask keyed
        // on these indices, so renumbering the existing entries would silently
        // remap every pad's buttons.
        L2, R2,
        // Right-stick vertical directions (wheel-like scrolling). Persisted
        // generic codes stay stable across this append: genericButton() indexes
        // relative to GenericButtonBase on both the emit and the persist side.
        RStickUp, RStickDown,
        // Thumbstick clicks. GameInput has always published these as the
        // canonical gamepad.thumb_left / gamepad.thumb_right; the legacy
        // backends used to emit them as unnamed generic buttons, which made
        // the SAME physical button resolve to a different canonical control
        // depending on which API happened to deliver the edge.
        L3, R3,
        ButtonCount
    };
    Q_ENUM(Button)

    // Indices >= GenericButtonBase are backend-reported buttons with no known
    // canonical position (see docs/controller-input.md): still bindable,
    // just labeled "Button N" instead of a named position. The quint32 edge
    // bitmask backends already build caps this range at 32 bits.
    static constexpr int GenericButtonBase = ButtonCount;
    static constexpr int MaxButtons = 32;

    explicit Gamepad(QObject* parent = nullptr) : QObject(parent) {}
    ~Gamepad() override = default;

    // Begin listening for input. Returns false only on a hard setup failure;
    // "no pad connected" is NOT a failure (returns true, emits nothing).
    virtual bool start() = 0;

    // Device metadata for the pad currently driving input on this backend.
    // Base implementation is a safe empty/generic default; backends that can
    // identify their active device (VID/PID, layout) override this.
    virtual ControlId::DeviceProfile profile() const { return {}; }

    static QString buttonName(int b)
    {
        switch (b) {
        case Share:     return QStringLiteral("Share");
        case Options:   return QStringLiteral("Options");
        case PS:        return QStringLiteral("PS");
        case Cross:     return QStringLiteral("Cross");
        case Circle:    return QStringLiteral("Circle");
        case Triangle:  return QStringLiteral("Triangle");
        case Square:    return QStringLiteral("Square");
        case L1:        return QStringLiteral("L1");
        case R1:        return QStringLiteral("R1");
        case L2:        return QStringLiteral("L2");
        case R2:        return QStringLiteral("R2");
        case DpadUp:    return QStringLiteral("D-Up");
        case DpadDown:  return QStringLiteral("D-Down");
        case DpadLeft:  return QStringLiteral("D-Left");
        case DpadRight: return QStringLiteral("D-Right");
        case RStickUp:   return QStringLiteral("RS-Up");
        case RStickDown: return QStringLiteral("RS-Down");
        case L3:        return QStringLiteral("L3");
        case R3:        return QStringLiteral("R3");
        default:
            // Matches ControlId::label()'s numbering for the same generic code.
            return b >= GenericButtonBase
                ? QStringLiteral("Button %1").arg(b - GenericButtonBase)
                : QStringLiteral("?");
        }
    }

    // Canonical, device-neutral code for a button index — what bindings
    // persist. Positions map physically (Cross/A/B share "face_south"); an
    // index with no known position falls back to a stable per-device generic
    // code so unmapped WinMM/XInput buttons stay bindable.
    static QString controlIdFor(int b)
    {
        switch (b) {
        case Cross:     return ControlId::FaceSouth;
        case Circle:    return ControlId::FaceEast;
        case Triangle:  return ControlId::FaceNorth;
        case Square:    return ControlId::FaceWest;
        case L1:        return ControlId::ShoulderLeft;
        case R1:        return ControlId::ShoulderRight;
        case L2:        return ControlId::TriggerLeft;
        case R2:        return ControlId::TriggerRight;
        case DpadUp:    return ControlId::DpadUp;
        case DpadDown:  return ControlId::DpadDown;
        case DpadLeft:  return ControlId::DpadLeft;
        case DpadRight: return ControlId::DpadRight;
        case Options:   return ControlId::Menu;
        case PS:        return ControlId::Guide;
        case Share:     return ControlId::Capture;
        case RStickUp:   return ControlId::StickRightUp;
        case RStickDown: return ControlId::StickRightDown;
        case L3:        return ControlId::ThumbLeft;
        case R3:        return ControlId::ThumbRight;
        default:
            return b >= GenericButtonBase
                ? ControlId::genericButton(b - GenericButtonBase)
                : QString();
        }
    }

signals:
    // Canonical backend events consumed by the binding runtime.
    void controlPressed(const QString& controlId, int family,
                        const QString& backend, const QString& fingerprint,
                        const QString& displayName);
    void controlReleased(const QString& controlId, int family,
                         const QString& backend, const QString& fingerprint,
                         const QString& displayName);
    void connected(bool isConnected);

protected:
    // Direct publication is the canonical backend path. Modern providers use
    // stable string IDs because their device-local controls are not limited by
    // the legacy 32-bit button mask.
    void publishControlPressed(const QString& controlId)
    {
        publishControlPressed(controlId, profile());
    }

    void publishControlPressed(const QString& controlId,
                               const ControlId::DeviceProfile& device)
    {
        if (controlId.isEmpty())
            return;
        emit controlPressed(controlId, static_cast<int>(device.family),
                            device.backend, device.fingerprint, device.displayName);
    }

    void publishControlReleased(const QString& controlId)
    {
        publishControlReleased(controlId, profile());
    }

    void publishControlReleased(const QString& controlId,
                                const ControlId::DeviceProfile& device)
    {
        if (controlId.isEmpty())
            return;
        emit controlReleased(controlId, static_cast<int>(device.family),
                             device.backend, device.fingerprint, device.displayName);
    }

    // Existing integer backends remain adapters; their enum values and bitmask
    // layouts stay byte-for-byte compatible.
    void publishButtonPressed(int button)
    {
        publishControlPressed(controlIdFor(button));
    }

    void publishButtonReleased(int button)
    {
        publishControlReleased(controlIdFor(button));
    }
};

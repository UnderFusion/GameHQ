#pragma once

#include "gameinput/GameInputEvent.h"

#include "GameInput.h"

namespace ModernInput::GameInputLabelMap {

namespace GI = GameInput::v3;

inline GameInputButtonClassification classification(GI::GameInputLabel label)
{
    switch (label) {
    case GI::GameInputLabelXboxGuide:
    case GI::GameInputLabelIconBranding:
    case GI::GameInputLabelIconHome:
    case GI::GameInputLabelHome:
    case GI::GameInputLabelGuide:
    case GI::GameInputLabelMode:
    case GI::GameInputLabelShare:
        return GameInputButtonClassification::System;

    case GI::GameInputLabelXboxBack:
    case GI::GameInputLabelXboxStart:
    case GI::GameInputLabelXboxMenu:
    case GI::GameInputLabelXboxView:
    case GI::GameInputLabelXboxA:
    case GI::GameInputLabelXboxB:
    case GI::GameInputLabelXboxX:
    case GI::GameInputLabelXboxY:
    case GI::GameInputLabelXboxDPadUp:
    case GI::GameInputLabelXboxDPadDown:
    case GI::GameInputLabelXboxDPadLeft:
    case GI::GameInputLabelXboxDPadRight:
    case GI::GameInputLabelXboxLeftShoulder:
    case GI::GameInputLabelXboxLeftTrigger:
    case GI::GameInputLabelXboxLeftStickButton:
    case GI::GameInputLabelXboxRightShoulder:
    case GI::GameInputLabelXboxRightTrigger:
    case GI::GameInputLabelXboxRightStickButton:
    case GI::GameInputLabelXboxPaddle1:
    case GI::GameInputLabelXboxPaddle2:
    case GI::GameInputLabelXboxPaddle3:
    case GI::GameInputLabelXboxPaddle4:
    case GI::GameInputLabelLetterA:
    case GI::GameInputLabelLetterB:
    case GI::GameInputLabelLetterC:
    case GI::GameInputLabelLetterX:
    case GI::GameInputLabelLetterY:
    case GI::GameInputLabelLetterZ:
    case GI::GameInputLabelArrowUp:
    case GI::GameInputLabelArrowRight:
    case GI::GameInputLabelArrowDown:
    case GI::GameInputLabelArrowLeft:
    case GI::GameInputLabelIconMenu:
    case GI::GameInputLabelIconCross:
    case GI::GameInputLabelIconCircle:
    case GI::GameInputLabelIconSquare:
    case GI::GameInputLabelIconTriangle:
    case GI::GameInputLabelIconDPadUp:
    case GI::GameInputLabelIconDPadDown:
    case GI::GameInputLabelIconDPadLeft:
    case GI::GameInputLabelIconDPadRight:
    case GI::GameInputLabelIconPlus:
    case GI::GameInputLabelIconMinus:
    case GI::GameInputLabelSelect:
    case GI::GameInputLabelMenu:
    case GI::GameInputLabelView:
    case GI::GameInputLabelBack:
    case GI::GameInputLabelStart:
    case GI::GameInputLabelOptions:
    case GI::GameInputLabelUp:
    case GI::GameInputLabelDown:
    case GI::GameInputLabelLeft:
    case GI::GameInputLabelRight:
    case GI::GameInputLabelLB:
    case GI::GameInputLabelLT:
    case GI::GameInputLabelLSB:
    case GI::GameInputLabelL1:
    case GI::GameInputLabelL2:
    case GI::GameInputLabelL3:
    case GI::GameInputLabelRB:
    case GI::GameInputLabelRT:
    case GI::GameInputLabelRSB:
    case GI::GameInputLabelR1:
    case GI::GameInputLabelR2:
    case GI::GameInputLabelR3:
    case GI::GameInputLabelPaddleLeft1:
    case GI::GameInputLabelPaddleLeft2:
    case GI::GameInputLabelPaddleRight1:
    case GI::GameInputLabelPaddleRight2:
        return GameInputButtonClassification::Standard;

    case GI::GameInputLabelUnknown:
    case GI::GameInputLabelNone:
        return GameInputButtonClassification::Unknown;

    default:
        return GameInputButtonClassification::Extra;
    }
}

inline QString normalizedLabel(GI::GameInputLabel label)
{
    const int value = int(label);
    if (value >= int(GI::GameInputLabelLetterA)
        && value <= int(GI::GameInputLabelLetterZ)) {
        return QString(QChar(u'A' + value - int(GI::GameInputLabelLetterA)));
    }
    if (value >= int(GI::GameInputLabelNumber0)
        && value <= int(GI::GameInputLabelNumber9)) {
        return QString(QChar(u'0' + value - int(GI::GameInputLabelNumber0)));
    }

    switch (label) {
    case GI::GameInputLabelUnknown:
    case GI::GameInputLabelNone: return {};
    case GI::GameInputLabelXboxGuide: return QStringLiteral("Xbox Guide");
    case GI::GameInputLabelXboxBack: return QStringLiteral("Xbox Back");
    case GI::GameInputLabelXboxStart: return QStringLiteral("Xbox Start");
    case GI::GameInputLabelXboxMenu: return QStringLiteral("Xbox Menu");
    case GI::GameInputLabelXboxView: return QStringLiteral("Xbox View");
    case GI::GameInputLabelXboxA: return QStringLiteral("A");
    case GI::GameInputLabelXboxB: return QStringLiteral("B");
    case GI::GameInputLabelXboxX: return QStringLiteral("X");
    case GI::GameInputLabelXboxY: return QStringLiteral("Y");
    case GI::GameInputLabelXboxDPadUp: return QStringLiteral("D-pad Up");
    case GI::GameInputLabelXboxDPadDown: return QStringLiteral("D-pad Down");
    case GI::GameInputLabelXboxDPadLeft: return QStringLiteral("D-pad Left");
    case GI::GameInputLabelXboxDPadRight: return QStringLiteral("D-pad Right");
    case GI::GameInputLabelXboxLeftShoulder: return QStringLiteral("Left Shoulder");
    case GI::GameInputLabelXboxLeftTrigger: return QStringLiteral("Left Trigger");
    case GI::GameInputLabelXboxLeftStickButton: return QStringLiteral("Left Stick Button");
    case GI::GameInputLabelXboxRightShoulder: return QStringLiteral("Right Shoulder");
    case GI::GameInputLabelXboxRightTrigger: return QStringLiteral("Right Trigger");
    case GI::GameInputLabelXboxRightStickButton: return QStringLiteral("Right Stick Button");
    case GI::GameInputLabelXboxPaddle1: return QStringLiteral("Xbox Paddle 1");
    case GI::GameInputLabelXboxPaddle2: return QStringLiteral("Xbox Paddle 2");
    case GI::GameInputLabelXboxPaddle3: return QStringLiteral("Xbox Paddle 3");
    case GI::GameInputLabelXboxPaddle4: return QStringLiteral("Xbox Paddle 4");
    case GI::GameInputLabelArrowUp: return QStringLiteral("Arrow Up");
    case GI::GameInputLabelArrowUpRight: return QStringLiteral("Arrow Up Right");
    case GI::GameInputLabelArrowRight: return QStringLiteral("Arrow Right");
    case GI::GameInputLabelArrowDownRight: return QStringLiteral("Arrow Down Right");
    case GI::GameInputLabelArrowDown: return QStringLiteral("Arrow Down");
    case GI::GameInputLabelArrowDownLLeft: return QStringLiteral("Arrow Down Left");
    case GI::GameInputLabelArrowLeft: return QStringLiteral("Arrow Left");
    case GI::GameInputLabelArrowUpLeft: return QStringLiteral("Arrow Up Left");
    case GI::GameInputLabelArrowUpDown: return QStringLiteral("Arrow Up Down");
    case GI::GameInputLabelArrowLeftRight: return QStringLiteral("Arrow Left Right");
    case GI::GameInputLabelArrowUpDownLeftRight: return QStringLiteral("Four-way Arrow");
    case GI::GameInputLabelArrowClockwise: return QStringLiteral("Arrow Clockwise");
    case GI::GameInputLabelArrowCounterClockwise: return QStringLiteral("Arrow Counter-clockwise");
    case GI::GameInputLabelArrowReturn: return QStringLiteral("Return");
    case GI::GameInputLabelIconBranding: return QStringLiteral("Branding");
    case GI::GameInputLabelIconHome: return QStringLiteral("Home");
    case GI::GameInputLabelIconMenu: return QStringLiteral("Menu");
    case GI::GameInputLabelIconCross: return QStringLiteral("Cross");
    case GI::GameInputLabelIconCircle: return QStringLiteral("Circle");
    case GI::GameInputLabelIconSquare: return QStringLiteral("Square");
    case GI::GameInputLabelIconTriangle: return QStringLiteral("Triangle");
    case GI::GameInputLabelIconStar: return QStringLiteral("Star");
    case GI::GameInputLabelIconDPadUp: return QStringLiteral("D-pad Up");
    case GI::GameInputLabelIconDPadDown: return QStringLiteral("D-pad Down");
    case GI::GameInputLabelIconDPadLeft: return QStringLiteral("D-pad Left");
    case GI::GameInputLabelIconDPadRight: return QStringLiteral("D-pad Right");
    case GI::GameInputLabelIconDialClockwise: return QStringLiteral("Dial Clockwise");
    case GI::GameInputLabelIconDialCounterClockwise: return QStringLiteral("Dial Counter-clockwise");
    case GI::GameInputLabelIconSliderLeftRight: return QStringLiteral("Horizontal Slider");
    case GI::GameInputLabelIconSliderUpDown: return QStringLiteral("Vertical Slider");
    case GI::GameInputLabelIconWheelUpDown: return QStringLiteral("Wheel");
    case GI::GameInputLabelIconPlus: return QStringLiteral("+");
    case GI::GameInputLabelIconMinus: return QStringLiteral("-");
    case GI::GameInputLabelIconSuspension: return QStringLiteral("Suspension");
    case GI::GameInputLabelHome: return QStringLiteral("Home");
    case GI::GameInputLabelGuide: return QStringLiteral("Guide");
    case GI::GameInputLabelMode: return QStringLiteral("Mode");
    case GI::GameInputLabelSelect: return QStringLiteral("Select");
    case GI::GameInputLabelMenu: return QStringLiteral("Menu");
    case GI::GameInputLabelView: return QStringLiteral("View");
    case GI::GameInputLabelBack: return QStringLiteral("Back");
    case GI::GameInputLabelStart: return QStringLiteral("Start");
    case GI::GameInputLabelOptions: return QStringLiteral("Options");
    case GI::GameInputLabelShare: return QStringLiteral("Share");
    case GI::GameInputLabelUp: return QStringLiteral("Up");
    case GI::GameInputLabelDown: return QStringLiteral("Down");
    case GI::GameInputLabelLeft: return QStringLiteral("Left");
    case GI::GameInputLabelRight: return QStringLiteral("Right");
    case GI::GameInputLabelLB: return QStringLiteral("LB");
    case GI::GameInputLabelLT: return QStringLiteral("LT");
    case GI::GameInputLabelLSB: return QStringLiteral("LSB");
    case GI::GameInputLabelL1: return QStringLiteral("L1");
    case GI::GameInputLabelL2: return QStringLiteral("L2");
    case GI::GameInputLabelL3: return QStringLiteral("L3");
    case GI::GameInputLabelRB: return QStringLiteral("RB");
    case GI::GameInputLabelRT: return QStringLiteral("RT");
    case GI::GameInputLabelRSB: return QStringLiteral("RSB");
    case GI::GameInputLabelR1: return QStringLiteral("R1");
    case GI::GameInputLabelR2: return QStringLiteral("R2");
    case GI::GameInputLabelR3: return QStringLiteral("R3");
    case GI::GameInputLabelPaddleLeft1: return QStringLiteral("Left Paddle 1");
    case GI::GameInputLabelPaddleLeft2: return QStringLiteral("Left Paddle 2");
    case GI::GameInputLabelPaddleRight1: return QStringLiteral("Right Paddle 1");
    case GI::GameInputLabelPaddleRight2: return QStringLiteral("Right Paddle 2");
    default: break; // letters and digits are mapped by range above
    }
    return QStringLiteral("GameInput Label %1").arg(value);
}

inline GameInputButtonDescriptor describe(GI::GameInputLabel label)
{
    return {qint32(label), normalizedLabel(label), classification(label)};
}

inline bool isExtra(const GameInputButtonDescriptor& descriptor)
{
    return descriptor.classification == GameInputButtonClassification::Unknown
        || descriptor.classification == GameInputButtonClassification::Extra;
}

} // namespace ModernInput::GameInputLabelMap

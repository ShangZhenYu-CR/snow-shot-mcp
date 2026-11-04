// SPDX-License-Identifier: GPL-3.0-or-later
// Added 2026-11-04 for the snow-shot-mcp modified source distribution.
#pragma once

#include "snow_shot/presentation/globalshortcuttypes.h"
#include "snow_shot/presentation/screenshotcontroller.h"
#include <QMap>

namespace snow_shot::app::mcp {
// Shared by discovery, external dispatch and the floating toolbar. These actions
// start a native selection workflow; the existing editor tools remain independent.
inline const QMap<QString, ScreenshotController::CaptureAction>& nativeSelectionActions() {
    using Action = ScreenshotController::CaptureAction;
    static const QMap<QString, Action> actions{
        {QStringLiteral("scrolling-screenshot"), Action::StartScrolling},
        {QStringLiteral("table-recognition"), Action::RecognizeTable},
        {QStringLiteral("barcode-recognition"), Action::RecognizeQr},
        {QStringLiteral("latex-recognition"), Action::RecognizeFormula},
        {QStringLiteral("convert-to-markdown"), Action::ConvertMarkdown},
        {QStringLiteral("convert-to-html"), Action::ConvertHtml},
        {QStringLiteral("save-as-file"), Action::Save},
        {QStringLiteral("print"), Action::Print},
        {QStringLiteral("airdrop"), Action::AirDrop},
        {QStringLiteral("upload-to-cloud"), Action::UploadToCloud},
        {QStringLiteral("pin-window-to-screen"), Action::PinWindow},
    };
    return actions;
}

inline bool nativeActionRequiresCapture(presentation::GlobalShortcutAction action) {
    using Action = presentation::GlobalShortcutAction;
    switch (action) {
    case Action::Screenshot:
    case Action::ScreenshotDelay:
    case Action::ScreenshotFixed:
    case Action::ScreenshotOcr:
    case Action::ScreenshotOcrCopy:
    case Action::ScreenshotTranslation:
    case Action::ScreenshotCopy:
    case Action::ScreenshotSave:
    case Action::ScreenshotQuickSave:
    case Action::ScreenshotFullScreen:
    case Action::ScreenshotFocusedWindow:
    case Action::ScreenRecord:
        return true;
    default:
        return false;
    }
}
} // namespace snow_shot::app::mcp

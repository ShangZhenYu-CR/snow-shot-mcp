// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QJsonObject>
#ifdef Q_OS_MACOS
#include "snow_shot/platform/macos/screenshotsessionfocus.h"
#endif

class ScreenshotController;
class ScreenshotOverlayWindow;
class ScreenshotToolbarCommandSink;
struct ScreenshotSelectionParams;

// Implemented only in test builds, where the controller's private implementation is complete.
class ScreenshotControllerTestAccess {
  public:
    static void prepareSelection(ScreenshotController& controller, QImage image,
                                 const ScreenshotSelectionParams& selection);
    static ScreenshotOverlayWindow* presentSelection(ScreenshotController& controller);
    static void presentMcpCapture(ScreenshotController& controller, const QJsonObject& options);
    static int delayedCaptureRemaining(const ScreenshotController& controller);
    [[nodiscard]] static bool exportActive(const ScreenshotController& controller);
#ifdef Q_OS_MACOS
    static void
    installSessionFocus(ScreenshotController& controller,
                        snow_shot::platform::macos::ScreenshotSessionFocus::Capture capture);
    static ScreenshotToolbarCommandSink& commands(ScreenshotController& controller);
#endif
};

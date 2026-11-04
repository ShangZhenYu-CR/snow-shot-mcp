#include "snow_shot/presentation/screenshotcontroller.h"
#include "snow_shot/presentation/directcapturecontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/presentation/screenshotselectionparams.h"
#include "screenshot_controller_test_access.h"

#include "widgets/platform_compatibility.h"

#include <QApplication>
#include <QEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QJsonArray>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void directCaptureDestructionKeepsControllerAvailableToActivityObservers() {
    for (const bool explicitShutdown : {false, true}) {
        auto controller = std::make_unique<snow_shot::presentation::DirectCaptureController>();
        QObject observer;
        int inactiveNotifications = 0;
        auto* retiring = controller.get();
        QObject::connect(
            retiring, &snow_shot::presentation::DirectCaptureController::captureActivityChanged,
            &observer, [&](const QString&, bool active) {
                require(!active && !retiring->blocksApplicationUpdate(),
                        "direct capture shutdown observers can inspect the inactive controller");
                ++inactiveNotifications;
            });
        if (explicitShutdown) {
            controller->shutdown();
        }
        controller.reset();
        require(inactiveNotifications == 2,
                "direct capture shutdown must release each activity source exactly once");
    }
}
void mcpPresentationDoesNotConfirmInteractiveSelection() {
    for (const bool interactive : {false, true}) {
        for (const bool monitor : {false, true}) {
            ScreenshotController controller;
            QImage image(320, 200, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::blue);
            ScreenshotControllerTestAccess::prepareSelection(controller, image, {});
            int presented = 0;
            QObject::connect(&controller, &ScreenshotController::mcpCapturePresented, &controller,
                             [&] { ++presented; });
            QJsonObject options{{QStringLiteral("presentation"), QStringLiteral("visible")}};
            if (interactive)
                options.insert(QStringLiteral("selection_mode"), QStringLiteral("interactive"));
            if (monitor) {
                options.insert(QStringLiteral("target"), QStringLiteral("monitor"));
                options.insert(QStringLiteral("monitor_id"), QStringLiteral("pin-copy-fixture"));
            }
            ScreenshotControllerTestAccess::presentMcpCapture(controller, options);
            const auto state = controller.mcpState();
            require(presented == 1, "each presentation completes the begin exactly once");
            require(state.value(QStringLiteral("capture_phase")).toString() ==
                        (interactive ? QStringLiteral("selecting") : QStringLiteral("editing")),
                    "interactive capture waits for selection; legacy capture enters editing");
            require(state.value(QStringLiteral("selection"))
                            .toObject()
                            .value(QStringLiteral("confirmed"))
                            .toBool() == !interactive,
                    "a target candidate is not a confirmed selection");
            if (interactive) {
                require(!controller.mcpExportArtifact(1), "unconfirmed candidates cannot export");
                QString error;
                require(controller.mcpSetSelection(
                            {{QStringLiteral("type"), QStringLiteral("rectangle")},
                             {QStringLiteral("bounds"), QJsonArray{10, 10, 80, 60}}},
                            &error),
                        "interactive sessions also accept explicit client selections");
                require(controller.mcpState()
                            .value(QStringLiteral("selection"))
                            .toObject()
                            .value(QStringLiteral("confirmed"))
                            .toBool(),
                        "explicit selection confirms the region");
            }
        }
    }
}
void nativeDelayHonorsPerCallDuration() {
    ScreenshotController controller;
    controller.startDelayedCapture(20);
    const auto remaining = ScreenshotControllerTestAccess::delayedCaptureRemaining(controller);
    require(remaining > 15000 && remaining <= 21000,
            "native timer accepts delays beyond the UI settings range");
    controller.startDelayedCapture(0);
    require(ScreenshotControllerTestAccess::delayedCaptureRemaining(controller) == 0,
            "zero delay schedules immediately and replaces the earlier timer");
    // Prevent real screen acquisition when the zero timer fires in this offscreen test.
    controller.setCaptureSuspended(true);
    QCoreApplication::processEvents();
    require(ScreenshotControllerTestAccess::delayedCaptureRemaining(controller) == -1,
            "delayed timer is single-shot and respects capture suspension");
}
} // namespace

// Linked only into test builds to exercise the real controller and all its dependencies.
// Entered before singleton acquisition or diagnostics can access the user's configuration.
int runScreenshotControllerLifetimeTests(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    adqt::widgets::initializePlatformCompatibility(application);
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "controller lifetime tests require isolated storage");

    directCaptureDestructionKeepsControllerAvailableToActivityObservers();
    mcpPresentationDoesNotConfirmInteractiveSelection();
    nativeDelayHonorsPerCallDuration();
    auto controller = std::make_unique<ScreenshotController>();
    QObject observer;
    int inactiveNotifications = 0;
    auto* retiring = controller.get();
    QObject::connect(
        retiring, &ScreenshotController::captureActivityChanged, &observer,
        [&](const QString&, bool active) {
            require(!active && !retiring->captureAcquisitionActive(),
                    "final capture activity observers can inspect the inactive controller");
            static_cast<void>(retiring->captureAvailable());
            static_cast<void>(retiring->blocksMemoryTrimming());
            ++inactiveNotifications;
        });
    controller.reset();
    require(inactiveNotifications > 0, "controller destruction must release capture activity");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    storage.shutdown();
    std::cout << "Screenshot controller lifetime tests passed\n";
    return 0;
}

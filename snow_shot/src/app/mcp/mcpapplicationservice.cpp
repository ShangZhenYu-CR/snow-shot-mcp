#include "snow_shot/app/mcp/mcpedition.h"
#include "snow_shot/app/mcp/mcpapplicationservice.h"
#include "snow_shot/app/mcp/mcpnativeactions.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/app/mcp/mcpjobregistry.h"
#include "snow_shot/app/mcp/screenshotmcpsession.h"
#include "mcpsettingsadapter_p.h"
#include "mcpasyncwork_p.h"
#include "mcpreplayregistry_p.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/configurationarchive.h"
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/translation/translationlanguages.h"
#endif
#include "snow_shot/update/updateservice.h"
#include "snow_shot/platform/windows/monitorgeometry.h"
#include "snowimageqtcodec.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QScreen>
#include <QSysInfo>
#include <QThreadPool>
#include <QTranslator>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <atomic>
#include <limits>

namespace snow_shot::app::mcp {
namespace {
QJsonArray rectangle(const QRect& rect) {
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}

// These are invocation recipes over the public tools, not shortcut simulation.
// Unknown native actions remain discoverable without advertising a fabricated adapter.
QJsonObject nativeActionContract(presentation::GlobalShortcutAction action) {
    using Action = presentation::GlobalShortcutAction;
    const auto invocation = [](const QString& tool, QJsonObject arguments) {
        return QJsonObject{{QStringLiteral("tool"), tool},
                           {QStringLiteral("arguments"), std::move(arguments)}};
    };
    const auto appAction = [&](const QString& name) {
        return QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("standalone")},
            {QStringLiteral("invocation"), invocation(QStringLiteral("snow_shot_app_action"),
                                                      {{QStringLiteral("action"), name}})}};
    };
    switch (action) {
    case Action::OpenSettings:
        return appAction(QStringLiteral("show_settings"));
    case Action::OpenCaptureHistory:
        return appAction(QStringLiteral("show_history"));
    case Action::OpenPinToScreenManagement:
        return appAction(QStringLiteral("show_pinned"));
    case Action::SwitchWindowGroup:
        return appAction(QStringLiteral("show_window_groups"));
    case Action::OpenScreenRecordingFolder:
        return appAction(QStringLiteral("open_recording_folder"));
    case Action::ScreenshotFullScreen:
    case Action::ScreenshotFocusedWindow:
        return {{QStringLiteral("kind"), QStringLiteral("standalone")},
                {QStringLiteral("requires_capture"), true},
                {QStringLiteral("invocation"),
                 invocation(QStringLiteral("snow_shot_screenshot_direct_capture"),
                            {{QStringLiteral("target"), action == Action::ScreenshotFullScreen
                                                            ? QStringLiteral("current_monitor")
                                                            : QStringLiteral("focused_window")},
                             {QStringLiteral("output"), QStringLiteral("copy")}})}};
    default:
        break;
    }
    QString continuation;
    QJsonObject arguments;
    QJsonArray prerequisites;
    switch (action) {
    case Action::Screenshot:
        break;
    case Action::ScreenshotFixed:
        continuation = QStringLiteral("snow_shot_screenshot_pin");
        break;
    case Action::ScreenshotCopy:
        continuation = QStringLiteral("snow_shot_screenshot_copy");
        break;
    case Action::ScreenshotQuickSave:
        continuation = QStringLiteral("snow_shot_screenshot_save");
        arguments.insert(QStringLiteral("automatic_path"), true);
        break;
    case Action::ScreenshotOcr:
        continuation = QStringLiteral("snow_shot_screenshot_recognize");
        arguments.insert(QStringLiteral("kind"), QStringLiteral("text"));
        prerequisites.append(QStringLiteral("ocr_provider"));
        break;
    case Action::ScreenshotTranslation:
        continuation = QStringLiteral("snow_shot_screenshot_translate");
        prerequisites.append(QStringLiteral("translation_provider"));
        break;
    default:
        return {{QStringLiteral("kind"), presentation::controlsGlobalHotkeyGates(action)
                                             ? QStringLiteral("administrative")
                                             : QStringLiteral("unmapped")}};
    }
    QJsonObject contract{
        {QStringLiteral("kind"),
         continuation.isEmpty() ? QStringLiteral("interactive") : QStringLiteral("workflow")},
        {QStringLiteral("requires_capture"), true},
        {QStringLiteral("prerequisites"), prerequisites},
        {QStringLiteral("invocation"),
         invocation(QStringLiteral("snow_shot_screenshot_begin"),
                    {{QStringLiteral("selection_mode"), QStringLiteral("interactive")}})}};
    if (!continuation.isEmpty()) {
        auto followup = invocation(continuation, arguments);
        followup.insert(QStringLiteral("when"), QStringLiteral("selection.confirmed"));
        followup.insert(
            QStringLiteral("bind_from_state"),
            QJsonObject{{QStringLiteral("session_id"), QStringLiteral("session_id")},
                        {QStringLiteral("expected_revision"), QStringLiteral("revision")}});
        contract.insert(QStringLiteral("continuation"), followup);
    }
    return contract;
}

ScreenshotMcpResponse reply(const ScreenshotMcpRequest& request, QJsonObject result = {}) {
    ScreenshotMcpResponse response;
    response.ok = true;
    response.requestId = request.requestId;
    response.result = std::move(result);
    if (response.result.contains(QStringLiteral("revision")))
        response.revision = response.result.value(QStringLiteral("revision")).toInteger();
    return response;
}

ScreenshotMcpResponse failure(const ScreenshotMcpRequest& request, const QString& code,
                              const QString& field = {}) {
    auto response = reply(request);
    response.ok = false;
    response.errorCode = code;
    response.errorMessage = code;
    if (!field.isEmpty())
        response.errorDetails.insert(QStringLiteral("field"), field);
    return response;
}

QJsonObject historyJson(const storage::CaptureHistoryRecord& record) {
    return {{QStringLiteral("history_id"), record.id},
            {QStringLiteral("created_utc"), record.createdUtc.toString(Qt::ISODateWithMs)},
            {QStringLiteral("canvas_bounds"), rectangle(record.canvasBounds)},
            {QStringLiteral("selection"), rectangle(record.selection.rectangle)},
            {QStringLiteral("bytes"), record.totalBytes},
            {QStringLiteral("source"), static_cast<int>(record.source)},
            {QStringLiteral("scrolling"), record.scrolling.value_or(false)},
            {QStringLiteral("has_image"), record.result.has_value()}};
}

QJsonObject storageJson(const storage::StorageStatus& state) {
    return {{QStringLiteral("read_available"), state.readAvailable},
            {QStringLiteral("write_available"), state.writeAvailable},
            {QStringLiteral("directory"), state.effectiveDirectory},
            {QStringLiteral("history_entries"), state.historyUsage.entryCount},
            {QStringLiteral("history_bytes"), state.historyUsage.totalBytes},
            {QStringLiteral("pinned_bytes"), state.appUsage.pinnedWindowBytes},
            {QStringLiteral("thumbnail_cache_bytes"), state.appUsage.thumbnailCacheBytes},
            {QStringLiteral("recording_temp_bytes"), state.appUsage.recordingTempBytes},
            {QStringLiteral("total_bytes"), state.appUsage.totalBytes()},
            {QStringLiteral("scanning"), state.appUsage.scanning},
            {QStringLiteral("maintenance_busy"),
             state.historyClearing || state.pinnedClearing || state.cacheClearing},
            {QStringLiteral("diagnostics_available"), state.diagnostics.loggingAvailable},
            {QStringLiteral("diagnostics_dropped_records"),
             static_cast<qint64>(state.diagnostics.droppedRecords)}};
}

QJsonObject permissionJson(presentation::AppPermissionService* service) {
    QJsonArray permissions;
    if (service) {
        static const QStringList names{QStringLiteral("checking"), QStringLiteral("granted"),
                                       QStringLiteral("missing"),  QStringLiteral("not_determined"),
                                       QStringLiteral("denied"),   QStringLiteral("restricted"),
                                       QStringLiteral("error")};
        for (int index = 0; index < 4; ++index) {
            const auto permission = static_cast<presentation::AppPermission>(index);
            permissions.append(
                QJsonObject{{QStringLiteral("id"), presentation::appPermissionId(permission)},
                            {QStringLiteral("status"),
                             names.value(static_cast<int>(service->snapshot().status(permission)))},
                            {QStringLiteral("pending"), service->requestPending(permission)}});
        }
    }
    return {{QStringLiteral("permissions"), permissions}};
}

QJsonObject updateJson(update::UpdateService* service) {
    if (!service)
        return {{QStringLiteral("state"), QStringLiteral("unavailable")}};
    const auto& status = service->status();
    static const QStringList states{
        QStringLiteral("unavailable"), QStringLiteral("idle"),        QStringLiteral("checking"),
        QStringLiteral("available"),   QStringLiteral("downloading"), QStringLiteral("verifying"),
        QStringLiteral("ready"),       QStringLiteral("applying"),    QStringLiteral("failed")};
    return {{QStringLiteral("state"), states.value(static_cast<int>(status.state))},
            {QStringLiteral("version"), status.version},
            {QStringLiteral("received_bytes"), status.received},
            {QStringLiteral("total_bytes"), status.total},
#ifdef Q_OS_MACOS
            {QStringLiteral("installation"), QStringLiteral("official_website")}
#else
            {QStringLiteral("installation"), service->managedByStore()
                                                 ? QStringLiteral("microsoft_store")
                                                 : QStringLiteral("native_updater")}
#endif
    };
}
} // namespace

struct McpApplicationService::Impl {
    McpApplicationService& q;
    Ports ports;
    QThreadPool workers;
    bool stopping = false;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    QHash<QString, QPointer<translation::TranslationJob>> translations;
#endif
    QString activeUpdateJob;
    quint64 activeUpdateOwner = 0;
    int admittedWork = 0;
    QHash<QString, std::shared_ptr<std::atomic_bool>> pendingReads;
    McpReplayRegistry requests;

    Impl(McpApplicationService& owner, Ports value)
        : q(owner), ports(std::move(value)), requests(owner, {}) {
        workers.setMaxThreadCount(2);
        workers.setExpiryTimeout(30000);
    }

    storage::ConfigurationStore& configuration() const {
        return ports.storage->configuration();
    }

    QJsonObject settingsSnapshot(const QString& section) const {
        QJsonArray fields;
        auto& session = *ports.settings;
        for (const auto& field : session.registry().fields()) {
            if (!section.isEmpty() && field.sectionId != section && field.pageId != section)
                continue;
            const auto state = session.state(field.id);
            QJsonObject item{{QStringLiteral("id"), field.id},
                             {QStringLiteral("page_id"), field.pageId},
                             {QStringLiteral("section_id"), field.sectionId},
                             {QStringLiteral("kind"), static_cast<int>(field.kind)},
                             {QStringLiteral("value"), settingsJson(state.acceptedValue)},
                             {QStringLiteral("enabled"), state.enabled},
                             {QStringLiteral("pending"), state.busy},
                             {QStringLiteral("failed"),
                              state.phase == settings::SettingsWritePhase::Failed ||
                                  state.phase == settings::SettingsWritePhase::Rejected},
                             {QStringLiteral("title"), field.definition->title.translated()}};
            if (const auto* schema = storage::ConfigurationSchema::entry(field.configurationKey)) {
                if (schema->integerRange)
                    item.insert(
                        QStringLiteral("range"),
                        QJsonObject{{QStringLiteral("minimum"), schema->integerRange->minimum},
                                    {QStringLiteral("maximum"), schema->integerRange->maximum},
                                    {QStringLiteral("step"), schema->integerRange->step}});
                if (!schema->allowedStringValues.isEmpty())
                    item.insert(QStringLiteral("allowed_values"),
                                QJsonArray::fromStringList(schema->allowedStringValues));
            }
            if (const auto* select =
                    std::get_if<settings::SettingsSelectDefinition>(&field.definition->payload)) {
                QJsonArray options;
                for (const auto& option : session.dynamicSelectOptions(select->binding))
                    options.append(
                        QJsonObject{{QStringLiteral("value"), settingsJson(option.value)},
                                    {QStringLiteral("label"), option.label}});
                item.insert(QStringLiteral("options"), options);
            }
            fields.append(item);
            if (const auto auxiliary = auxiliaryIntegerSetting(field)) {
                const int value = session.integerValue(auxiliary->binding);
                item.insert(QStringLiteral("id"), auxiliary->id);
                item.insert(QStringLiteral("parent_id"), field.id);
                item.insert(QStringLiteral("kind"),
                            static_cast<int>(settings::SettingsFieldKind::Integer));
                item.insert(QStringLiteral("value"), value);
                item.insert(QStringLiteral("title"),
                            field.definition->title.translated().arg(value));
                item.remove(QStringLiteral("options"));
                const auto* schema =
                    storage::ConfigurationSchema::entry(auxiliary->configurationKey);
                if (schema && schema->integerRange)
                    item.insert(
                        QStringLiteral("range"),
                        QJsonObject{{QStringLiteral("minimum"), schema->integerRange->minimum},
                                    {QStringLiteral("maximum"), schema->integerRange->maximum},
                                    {QStringLiteral("step"), schema->integerRange->step}});
                fields.append(item);
            }
        }
        return {{QStringLiteral("revision"), static_cast<qint64>(configuration().revision())},
                {QStringLiteral("fields"), fields}};
    }

    void observeSettings(const ScreenshotMcpRequest& request, const QString& settingsJob,
                         const QMap<QString, quint64>& pending, const QStringList& applied,
                         ScreenshotMcpResponse& response) {
        if (pending.isEmpty()) {
            ports.jobs->discard(request.connectionId, settingsJob);
        } else {
            auto& details = response.ok ? response.result : response.errorDetails;
            details.insert(QStringLiteral("job_id"), settingsJob);
            details.insert(QStringLiteral("pending_fields"),
                           QJsonArray::fromStringList(pending.keys()));
            auto* observation = new QObject(&q);
            const auto observe = [this, observation = QPointer<QObject>(observation), settingsJob,
                                  owner = request.connectionId, pending, applied] {
                const auto job = ports.jobs->get(owner, settingsJob);
                if (!job || job->value(QStringLiteral("status")) != QStringLiteral("running")) {
                    observation->deleteLater();
                    return;
                }
                auto completed = applied;
                for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
                    const auto state = ports.settings->state(it.key());
                    if (state.revision != it.value() || state.conflicted ||
                        state.phase == settings::SettingsWritePhase::Failed ||
                        state.phase == settings::SettingsWritePhase::Rejected) {
                        ports.jobs->fail(settingsJob, QStringLiteral("settings_failed"));
                        if (observation)
                            observation->deleteLater();
                        return;
                    }
                    if (state.busy || state.phase == settings::SettingsWritePhase::Pending)
                        return;
                    completed.append(it.key());
                }
                ports.jobs->complete(
                    settingsJob,
                    {{QStringLiteral("revision"), static_cast<qint64>(configuration().revision())},
                     {QStringLiteral("applied_fields"), QJsonArray::fromStringList(completed)}});
                if (observation)
                    observation->deleteLater();
            };
            QObject::connect(ports.settings, &settings::SettingsRuntimeSession::fieldChanged,
                             observation, [observe](const auto&, const auto&) { observe(); });
            QObject::connect(ports.jobs, &McpJobRegistry::changed, observation,
                             [this, observation, owner = request.connectionId,
                              settingsJob](quint64 changedOwner, const QString& changedId) {
                                 if (changedOwner == owner && changedId == settingsJob &&
                                     !ports.jobs->get(owner, settingsJob))
                                     observation->deleteLater();
                             });
            observe();
        }
    }

    // Work admitted here only uses immutable input and thread-safe storage APIs.
    void background(const ScreenshotMcpRequest& request,
                    std::function<ScreenshotMcpResponse()> work,
                    ScreenshotMcpServer::Completion completion, bool cancellable = true) {
        if (admittedWork >= 2) {
            completion(failure(request, QStringLiteral("queue_full")));
            return;
        }
        ++admittedWork;
        const QString key = QString::number(request.connectionId) + u':' + request.requestId;
        auto canceled = std::make_shared<std::atomic_bool>(false);
        if (cancellable)
            pendingReads.insert(key, canceled);
        auto* watcher = new QFutureWatcher<ScreenshotMcpResponse>(&q);
        QObject::connect(
            watcher, &QFutureWatcher<ScreenshotMcpResponse>::finished, &q,
            [this, watcher, request, key, canceled, completion = std::move(completion)]() mutable {
                auto response = watcher->result();
                watcher->deleteLater();
                --admittedWork;
                if (pendingReads.value(key) == canceled)
                    pendingReads.remove(key);
                if (stopping || canceled->load())
                    response = failure(request, QStringLiteral("canceled"));
                completion(std::move(response));
            });
        watcher->setFuture(runMcpWork(&workers, [request, canceled, work = std::move(work)] {
            if (canceled->load())
                return failure(request, QStringLiteral("canceled"));
            try {
                return work();
            } catch (...) {
                return failure(request, QStringLiteral("operation_failed"));
            }
        }));
    }

    void handle(const ScreenshotMcpRequest&, ScreenshotMcpServer::Completion);

#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    void translate(const QString& id, const QStringList& texts,
                   const translation::TranslationPreferences& preferences) {
        const QPointer<McpApplicationService> lifetime(&q);
        if (!ports.jobs->retainInput(
                id,
                {{QStringLiteral("texts"), QJsonArray::fromStringList(texts)},
                 {QStringLiteral("source_language"), preferences.sourceLanguage},
                 {QStringLiteral("target_language"), preferences.targetLanguage},
                 {QStringLiteral("secondary_target_language"), preferences.secondaryTargetLanguage},
                 {QStringLiteral("language_detection_confidence_threshold"),
                  preferences.languageDetectionConfidenceThreshold},
                 {QStringLiteral("model_id"), preferences.modelId}})) {
            ports.jobs->fail(id, QStringLiteral("capacity_exceeded"));
            return;
        }
        auto* job = ports.translation->createJob(texts, preferences, &q);
        const bool cancellationSet =
            ports.jobs->setCancellation(id, [guard = QPointer<translation::TranslationJob>(job)] {
                if (guard) {
                    guard->cancel();
                    guard->deleteLater();
                }
            });
        if (!lifetime)
            return;
        if (!cancellationSet) {
            delete job;
            return;
        }
        translations.insert(id, job);
        QObject::connect(job, &QObject::destroyed, &q, [this, id] { translations.remove(id); });
        auto snapshot = [job] {
            QJsonArray units;
            for (const auto& unit : job->units())
                units.append(QJsonObject{{QStringLiteral("source"), unit.sourceText},
                                         {QStringLiteral("text"), unit.text},
                                         {QStringLiteral("state"), static_cast<int>(unit.state)}});
            return QJsonObject{{QStringLiteral("units"), units}};
        };
        QObject::connect(
            job, &translation::TranslationJob::unitChanged, &q, [this, id, job](int index) {
                const auto units = job->units();
                if (index < 0 || index >= units.size())
                    return;
                const auto& unit = units.at(index);
                ports.jobs->progress(id, {{QStringLiteral("unit"), index},
                                          {QStringLiteral("unit_count"), units.size()},
                                          {QStringLiteral("state"), static_cast<int>(unit.state)},
                                          {QStringLiteral("text"), unit.text.left(2048)}});
            });
        auto done = [this, id, job = QPointer<translation::TranslationJob>(job), snapshot] {
            if (!translations.remove(id))
                return;
            if (job->state() == translation::TranslationJob::State::Completed)
                ports.jobs->complete(id, snapshot());
            else
                ports.jobs->fail(id, QStringLiteral("translation_failed"));
            if (job)
                job->deleteLater();
        };
        QObject::connect(job, &translation::TranslationJob::finished, &q, done);
        QObject::connect(job, &translation::TranslationJob::invalidated, &q, done);
        job->start();
    }

#endif

    void storageJob(const ScreenshotMcpRequest& request,
                    const std::function<std::shared_future<storage::StorageResult>()>& start,
                    ScreenshotMcpServer::Completion completion) {
        if (admittedWork >= 2) {
            completion(failure(request, QStringLiteral("queue_full")));
            return;
        }
        const QPointer<McpApplicationService> lifetime(&q);
        const auto id = ports.jobs->start(request.connectionId, request.method);
        if (!lifetime)
            return;
        if (id.isEmpty()) {
            completion(failure(request, QStringLiteral("queue_full")));
            return;
        }
        const auto future = start();
        if (!lifetime)
            return;
        completion(reply(request, {{QStringLiteral("job_id"), id}}));
        if (!lifetime)
            return;
        background(
            request,
            [future, request] {
                const auto result = future.get();
                return result.success ? reply(request, {{QStringLiteral("completed"), true}})
                                      : failure(request, result.error == u"stale_revision"
                                                             ? QStringLiteral("stale_revision")
                                                             : QStringLiteral("storage_failed"));
            },
            [this, id](ScreenshotMcpResponse result) {
                if (result.ok)
                    ports.jobs->complete(id, result.result);
                else
                    ports.jobs->fail(id, result.errorCode);
            },
            false);
    }
};

McpApplicationService::McpApplicationService(Ports ports, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(ports))) {}
McpApplicationService::~McpApplicationService() {
    shutdown();
    // Child destruction callbacks use Impl. QObject's base destructor runs only
    // after m_impl is gone, so retire the owned callback tree while state is valid.
    while (!children().isEmpty())
        delete children().constLast();
}

QStringList McpApplicationService::methods() {
    return {
        QStringLiteral("snow_shot_app_status"),
        QStringLiteral("snow_shot_app_displays"),
        QStringLiteral("snow_shot_app_actions"),
        QStringLiteral("snow_shot_app_action"),
        QStringLiteral("snow_shot_settings_get"),
        QStringLiteral("snow_shot_settings_update"),
        QStringLiteral("snow_shot_settings_reset"),
        QStringLiteral("snow_shot_settings_action"),
#if SNOW_SHOT_ENABLE_API_CONFIGURATION
        QStringLiteral("snow_shot_models_list"),
        QStringLiteral("snow_shot_models_update"),
        QStringLiteral("snow_shot_credentials_set"),
#endif
        QStringLiteral("snow_shot_history_list"),
        QStringLiteral("snow_shot_history_get"),
        QStringLiteral("snow_shot_history_delete"),
        QStringLiteral("snow_shot_history_clear"),
        QStringLiteral("snow_shot_history_action"),
        QStringLiteral("snow_shot_configuration_export"),
        QStringLiteral("snow_shot_configuration_import"),
        QStringLiteral("snow_shot_storage_status"),
        QStringLiteral("snow_shot_storage_cleanup"),
        QStringLiteral("snow_shot_permissions_get"),
        QStringLiteral("snow_shot_permissions_request"),
        QStringLiteral("snow_shot_updates_status"),
        QStringLiteral("snow_shot_updates_action"),
        QStringLiteral("snow_shot_templates_list"),
        QStringLiteral("snow_shot_templates_update"),
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        QStringLiteral("snow_shot_translation_catalog"),
        QStringLiteral("snow_shot_translation_start"),
#endif
    };
}
bool McpApplicationService::handles(const QString& method) const {
    return methods().contains(method);
}
void McpApplicationService::request(const ScreenshotMcpRequest& request,
                                    ScreenshotMcpServer::Completion completion) {
    if (m_impl->stopping || !m_impl->ports.storage || !m_impl->ports.settings) {
        completion(failure(request, QStringLiteral("unavailable")));
        return;
    }
    auto operation = m_impl->requests.begin(request, std::move(completion));
    if (operation)
        m_impl->handle(request, std::move(operation.complete));
}
void McpApplicationService::disconnected(quint64 connection) {
    m_impl->requests.disconnected(connection);
    const auto prefix = QString::number(connection) + u':';
    for (auto it = m_impl->pendingReads.begin(); it != m_impl->pendingReads.end(); ++it)
        if (it.key().startsWith(prefix))
            it.value()->store(true);
}
bool McpApplicationService::cancelRequest(quint64 connection, const QString& requestId) {
    if (m_impl->requests.cancelAlias(connection, requestId))
        return true;
    const auto canceled =
        m_impl->pendingReads.value(QString::number(connection) + u':' + requestId);
    if (!canceled)
        return false;
    canceled->store(true);
    return true;
}
void McpApplicationService::shutdown() {
    if (!m_impl || m_impl->stopping)
        return;
    m_impl->stopping = true;
    m_impl->requests.shutdown();
    m_impl->workers.waitForDone();
}

void McpApplicationService::Impl::handle(const ScreenshotMcpRequest& request,
                                         ScreenshotMcpServer::Completion completion) {
    const QPointer<McpApplicationService> lifetime(&q);
    const auto& method = request.method;
    const auto& params = request.params;
    const auto expected = request.expectedRevision;
    auto finish = [&](QJsonObject result) { completion(reply(request, std::move(result))); };
    auto error = [&](const QString& code, const QString& field = QString()) {
        completion(failure(request, code, field));
    };
    if (!editionRequestEnabled(method, params)) {
        error(QStringLiteral("unsupported"));
        return;
    }
    auto change = [&](const std::function<bool()>& mutation) {
        if (!expected) {
            error(QStringLiteral("revision_required"));
            return;
        }
        bool conflict = false;
        const bool ok = configuration().mutateIfRevision(*expected, mutation, &conflict);
        if (!ok) {
            auto response = failure(request, conflict ? QStringLiteral("stale_revision")
                                                      : QStringLiteral("operation_failed"));
            response.revision = configuration().revision();
            completion(std::move(response));
            return;
        }
        finish({{QStringLiteral("revision"), static_cast<qint64>(configuration().revision())}});
    };

    if (method == u"snow_shot_app_status") {
        finish(
            {{QStringLiteral("version"), QCoreApplication::applicationVersion()},
             {QStringLiteral("platform"), QSysInfo::productType()},
             {QStringLiteral("storage"), storageJson(ports.storage->status())},
             {QStringLiteral("settings_revision"), static_cast<qint64>(configuration().revision())},
             {QStringLiteral("permissions"), permissionJson(ports.permissions)},
             {QStringLiteral("updates"), updateJson(ports.updates)}});
    } else if (method == u"snow_shot_app_actions") {
        const auto locale = params.value(QStringLiteral("locale")).toString();
        QTranslator translator;
        if (!locale.isEmpty() && locale != QStringLiteral("en_US")) {
            static const QRegularExpression localePattern(
                QStringLiteral("^[A-Za-z][A-Za-z0-9]*(?:[_-][A-Za-z0-9]+)*$"));
            if (!localePattern.match(locale).hasMatch() ||
                !translator.load(QStringLiteral(":/i18n/snow_shot_") + locale +
                                 QStringLiteral(".qm"))) {
                error(QStringLiteral("unsupported_locale"), QStringLiteral("locale"));
                return;
            }
        }
        const auto translated = [&](const settings::TranslatableText& text) {
            if (!text.isValid())
                return QString();
            if (locale.isEmpty())
                return text.translated();
            const auto value = translator.translate(text.context, text.source);
            return value.isEmpty() ? QString::fromUtf8(text.source) : value;
        };
        QJsonArray actions;
        QString selectionCategoryName;
        const auto iconJson = [&](const adqt::icons::IconRef& icon) {
            const auto metadata = adqt::icons::describeIcon(icon);
            QJsonObject result{
                {QStringLiteral("id"),
                 metadata.key.pack + u':' + metadata.key.variant + u':' + metadata.key.name},
                {QStringLiteral("license_reference"), QStringLiteral("THIRD_PARTY_NOTICES.md")}};
            if (params.value(QStringLiteral("include_icons")).toBool()) {
                adqt::icons::IconRenderRequest render;
                render.logicalSize = QSize(32, 32);
                render.devicePixelRatio = 1;
                QByteArray bytes;
                QBuffer buffer(&bytes);
                buffer.open(QIODevice::WriteOnly);
                if (adqt::icons::renderIconImage(icon, render).save(&buffer, "PNG")) {
                    result.insert(QStringLiteral("mime_type"), QStringLiteral("image/png"));
                    result.insert(QStringLiteral("data_base64"),
                                  QString::fromLatin1(bytes.toBase64()));
                }
            }
            return result;
        };
        QSet<int> seen;
        for (const auto& page : ports.settings->registry().catalog().pages()) {
            for (const auto& section : page.sections) {
                for (const auto& item : section.items) {
                    const auto* native =
                        std::get_if<settings::SettingsShortcutActionDefinition>(&item.payload);
                    if (!native || seen.contains(static_cast<int>(native->shortcutAction)))
                        continue;
                    seen.insert(static_cast<int>(native->shortcutAction));
                    auto contract = nativeActionContract(native->shortcutAction);
                    using Native = presentation::GlobalShortcutAction;
                    const auto nativeAction = native->shortcutAction;
                    if (nativeAction == Native::Screenshot)
                        selectionCategoryName = translated(section.title);
                    const bool recordingToggle = nativeAction == Native::ScreenRecordCopy;
                    const auto recordingState = recordingToggle && ports.nativeRecordingState
                                                    ? ports.nativeRecordingState()
                                                    : QStringLiteral("unknown");
                    const bool recordingOpen =
                        recordingState == u"ready" || recordingState == u"recording";
                    const bool requiresCapture =
                        nativeActionRequiresCapture(nativeAction) ||
                        (recordingToggle && recordingState != u"recording");
                    contract.insert(QStringLiteral("requires_capture"), requiresCapture);
                    contract.insert(QStringLiteral("capture_state_dependent"), recordingToggle);
                    contract.insert(QStringLiteral("capture_behavior"),
                                    recordingToggle
                                        ? (recordingState == u"recording"
                                               ? QStringLiteral("stop_recording_and_copy")
                                           : recordingState == u"ready"
                                               ? QStringLiteral("start_recording")
                                           : recordingState == u"closed"
                                               ? QStringLiteral("start_recording_selection")
                                               : QStringLiteral("recording_toggle"))
                                    : nativeAction == Native::ScreenRecord
                                        ? QStringLiteral("start_recording_selection")
                                    : nativeAction == Native::ScreenshotDelay
                                        ? QStringLiteral("delayed_capture")
                                    : requiresCapture ? QStringLiteral("capture")
                                                      : QStringLiteral("none"));
                    if (recordingToggle)
                        contract.insert(
                            QStringLiteral("capture_behavior_by_state"),
                            QJsonObject{
                                {QStringLiteral("recording_closed"),
                                 QJsonObject{{QStringLiteral("requires_capture"), true},
                                             {QStringLiteral("capture_behavior"),
                                              QStringLiteral("start_recording_selection")}}},
                                {QStringLiteral("recording_ready"),
                                 QJsonObject{{QStringLiteral("requires_capture"), true},
                                             {QStringLiteral("capture_behavior"),
                                              QStringLiteral("start_recording")}}},
                                {QStringLiteral("recording_active"),
                                 QJsonObject{{QStringLiteral("requires_capture"), false},
                                             {QStringLiteral("capture_behavior"),
                                              QStringLiteral("stop_recording_and_copy")}}}});
                    auto prerequisites = contract.value(QStringLiteral("prerequisites")).toArray();
                    if (nativeAction == Native::ScreenshotOcrCopy)
                        prerequisites.append(QStringLiteral("ocr_provider"));
                    if (nativeAction == Native::TranslateSelectedText)
                        prerequisites.append(QStringLiteral("translation_provider"));
                    if (nativeAction == Native::ScreenshotTranslation)
                        prerequisites.append(QStringLiteral("ocr_provider"));
                    contract.insert(QStringLiteral("prerequisites"), prerequisites);
                    // Preserve explicit client orchestration alongside native user workflows.
                    if (contract.contains(QStringLiteral("invocation"))) {
                        QJsonObject client{{QStringLiteral("invocation"),
                                            contract.take(QStringLiteral("invocation"))}};
                        if (contract.contains(QStringLiteral("continuation")))
                            client.insert(QStringLiteral("continuation"),
                                          contract.take(QStringLiteral("continuation")));
                        contract.insert(QStringLiteral("client_workflow"), client);
                    }
                    const bool supported = bool(ports.nativeAction);
                    contract.insert(QStringLiteral("kind"),
                                    presentation::controlsGlobalHotkeyGates(native->shortcutAction)
                                        ? QStringLiteral("administrative")
                                    : nativeActionRequiresCapture(native->shortcutAction)
                                        ? QStringLiteral("interactive")
                                        : QStringLiteral("standalone"));
                    contract.insert(QStringLiteral("control_mode"), QStringLiteral("user"));
                    if (supported)
                        contract.insert(
                            QStringLiteral("invocation"),
                            QJsonObject{
                                {QStringLiteral("tool"), QStringLiteral("snow_shot_app_action")},
                                {QStringLiteral("arguments"),
                                 QJsonObject{{QStringLiteral("action"), QStringLiteral("native")},
                                             {QStringLiteral("action_id"), item.id}}}});
                    if (native->shortcutAction ==
                        presentation::GlobalShortcutAction::ScreenshotDelay)
                        contract.insert(
                            QStringLiteral("parameters"),
                            QJsonObject{
                                {QStringLiteral("delay_seconds"),
                                 QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                             {QStringLiteral("minimum"), 0},
                                             {QStringLiteral("maximum"),
                                              std::numeric_limits<int>::max() / 1000},
                                             {QStringLiteral("default_source"),
                                              QStringLiteral("screenshot/delay_seconds")}}}});
                    QString reason;
                    QJsonValue available = QJsonValue::Null;
                    const auto nativeReason =
                        ports.nativeActionUnavailable
                            ? ports.nativeActionUnavailable(native->shortcutAction)
                            : QString();
                    if (!supported) {
                        available = false;
                        reason = QStringLiteral("native_action_adapter_missing");
                    } else if (!nativeReason.isEmpty()) {
                        available = false;
                        reason = nativeReason;
                    } else if ((nativeActionRequiresCapture(nativeAction) ||
                                (recordingToggle && !recordingOpen)) &&
                               (!ports.screenshotAvailable || !ports.screenshotAvailable())) {
                        available = ports.screenshotAvailable ? QJsonValue(false)
                                                              : QJsonValue(QJsonValue::Null);
                        reason = ports.screenshotAvailable ? QStringLiteral("capture_busy")
                                                           : QStringLiteral("availability_unknown");
                    } else if (!contract.value(QStringLiteral("prerequisites"))
                                    .toArray()
                                    .isEmpty()) {
                        reason = QStringLiteral("provider_preconditions_not_evaluated");
                    } else {
                        available = true;
                    }
                    contract.insert(QStringLiteral("id"), item.id);
                    contract.insert(QStringLiteral("name"), translated(item.title));
                    contract.insert(QStringLiteral("name_en"),
                                    QString::fromUtf8(item.title.source));
                    contract.insert(QStringLiteral("description"), translated(item.description));
                    contract.insert(QStringLiteral("category"), section.id);
                    contract.insert(QStringLiteral("category_name"), translated(section.title));
                    contract.insert(QStringLiteral("supported"), supported);
                    contract.insert(QStringLiteral("available"), available);
                    contract.insert(QStringLiteral("unavailable_reason"), reason);
                    if (native->iconFactory)
                        contract.insert(QStringLiteral("icon"), iconJson(native->iconFactory()));
                    actions.append(contract);
                }
            }
        }
        for (const auto& descriptor : presentation::toolbar_layout::actionDescriptors()) {
            const auto id = QString::fromLatin1(descriptor.id);
            if (!nativeSelectionActions().contains(id))
                continue;
            const bool supported = bool(ports.nativeSelectionAction);
            QJsonArray prerequisites;
            if (id == u"table-recognition")
                prerequisites.append(QStringLiteral("table_recognition_provider"));
            else if (id == u"barcode-recognition")
                prerequisites.append(QStringLiteral("qr_decoder"));
            else if (id == u"latex-recognition")
                prerequisites.append(QStringLiteral("formula_recognition_provider"));
            else if (id == u"convert-to-markdown" || id == u"convert-to-html")
                prerequisites.append(QStringLiteral("image_conversion_provider"));
            else if (id == u"upload-to-cloud")
                prerequisites.append(QStringLiteral("cloud_upload_configuration"));
            else if (id == u"print")
                prerequisites.append(QStringLiteral("print_service"));
            else if (id == u"airdrop")
                prerequisites.append(QStringLiteral("airdrop_service"));
            QJsonValue available = QJsonValue::Null;
            QString reason;
            if (!supported) {
                available = false;
                reason = QStringLiteral("native_action_adapter_missing");
            } else if (!ports.screenshotAvailable) {
                reason = QStringLiteral("availability_unknown");
            } else if (!ports.screenshotAvailable()) {
                available = false;
                reason = QStringLiteral("capture_busy");
            } else if (!prerequisites.isEmpty()) {
                reason = QStringLiteral("provider_preconditions_not_evaluated");
            } else {
                available = true;
            }
            QJsonObject action{{QStringLiteral("id"), id},
                               {QStringLiteral("name"),
                                translated({descriptor.translationContext, descriptor.label})},
                               {QStringLiteral("name_en"), QString::fromUtf8(descriptor.label)},
                               {QStringLiteral("category"), QStringLiteral("selection")},
                               {QStringLiteral("category_name"), selectionCategoryName},
                               {QStringLiteral("icon"),
                                iconJson(presentation::toolbar_layout::icon(descriptor.icon))},
                               {QStringLiteral("prerequisites"), prerequisites},
                               {QStringLiteral("requires_capture"), true},
                               {QStringLiteral("capture_state_dependent"), false},
                               {QStringLiteral("capture_behavior"), QStringLiteral("capture")},
                               {QStringLiteral("kind"), QStringLiteral("interactive")},
                               {QStringLiteral("control_mode"), QStringLiteral("user")},
                               {QStringLiteral("supported"), supported},
                               {QStringLiteral("available"), available},
                               {QStringLiteral("unavailable_reason"), reason}};
            if (supported)
                action.insert(
                    QStringLiteral("invocation"),
                    QJsonObject{{QStringLiteral("tool"), QStringLiteral("snow_shot_app_action")},
                                {QStringLiteral("arguments"),
                                 QJsonObject{{QStringLiteral("action"), QStringLiteral("native")},
                                             {QStringLiteral("action_id"), id}}}});
            actions.append(action);
        }
        finish(
            {{QStringLiteral("catalog_version"), 2},
             {QStringLiteral("application_version"), QCoreApplication::applicationVersion()},
             {QStringLiteral("product"), edition::productId()},
             {QStringLiteral("locale"), locale.isEmpty() ? QStringLiteral("application") : locale},
             {QStringLiteral("actions"), actions}});
    } else if (method == u"snow_shot_app_displays") {
        QJsonArray displays;
        for (auto* screen : QGuiApplication::screens()) {
            auto display = QJsonObject{
                {QStringLiteral("id"), screen->name()},
                {QStringLiteral("name"), screen->name()},
                {QStringLiteral("logical_bounds"), rectangle(screen->geometry())},
                {QStringLiteral("scale"), screen->devicePixelRatio()},
                {QStringLiteral("primary"), screen == QGuiApplication::primaryScreen()}};
            const auto physical = platform::windows::nativeMonitorRect(*screen);
            if (!physical.isEmpty())
                display.insert(QStringLiteral("physical_bounds"), rectangle(physical));
            displays.append(display);
        }
        finish({{QStringLiteral("displays"), displays}});
    } else if (method == u"snow_shot_app_action") {
        const auto action = params.value(QStringLiteral("action")).toString();
        if (action == u"native") {
            const auto id = params.value(QStringLiteral("action_id")).toString();
            std::optional<presentation::GlobalShortcutAction> native;
            for (const auto& page : ports.settings->registry().catalog().pages())
                for (const auto& section : page.sections)
                    for (const auto& item : section.items)
                        if (item.id == id)
                            if (const auto* definition =
                                    std::get_if<settings::SettingsShortcutActionDefinition>(
                                        &item.payload))
                                native = definition->shortcutAction;
            const bool selection =
                nativeSelectionActions().contains(id) && editionActionToolAvailable(id);
            if (!native && !selection) {
                error(QStringLiteral("invalid_parameters"), QStringLiteral("action_id"));
                return;
            }
            if (params.contains(QStringLiteral("delay_seconds"))) {
                const auto delay = params.value(QStringLiteral("delay_seconds"));
                if (native != presentation::GlobalShortcutAction::ScreenshotDelay ||
                    !delay.isDouble() || delay.toDouble() < 0 ||
                    delay.toDouble() > std::numeric_limits<int>::max() / 1000 ||
                    std::floor(delay.toDouble()) != delay.toDouble()) {
                    error(QStringLiteral("invalid_parameters"), QStringLiteral("delay_seconds"));
                    return;
                }
            }
            if ((native && !ports.nativeAction) || (selection && !ports.nativeSelectionAction)) {
                error(QStringLiteral("unsupported"));
                return;
            }
            if (native && ports.nativeActionUnavailable) {
                const auto reason = ports.nativeActionUnavailable(*native);
                if (!reason.isEmpty()) {
                    error(reason);
                    return;
                }
            }
            if ((selection || (native && nativeActionRequiresCapture(*native))) &&
                (!ports.screenshotAvailable || !ports.screenshotAvailable())) {
                error(QStringLiteral("capture_busy"));
                return;
            }
            const auto reason =
                native ? ports.nativeAction(*native, params) : ports.nativeSelectionAction(id);
            if (!reason.isEmpty())
                error(reason);
            else
                finish({{QStringLiteral("accepted"), true},
                        {QStringLiteral("action_id"), id},
                        {QStringLiteral("control_mode"), QStringLiteral("user")},
                        {QStringLiteral("completion"), QStringLiteral("native_workflow")}});
        } else if (params.contains(QStringLiteral("action_id")) ||
                   params.contains(QStringLiteral("delay_seconds"))) {
            error(QStringLiteral("invalid_parameters"));
        } else if (action == u"restart" || action == u"quit") {
            if (!ports.restartAllowed || !ports.restartAllowed()) {
                error(QStringLiteral("busy"));
                return;
            }
            auto response = reply(request, {{QStringLiteral("accepted"), true}});
            response.afterSend = [guard = QPointer<McpApplicationService>(&q), action, params] {
                if (guard && guard->m_impl->ports.action)
                    guard->m_impl->ports.action(action, params);
            };
            completion(std::move(response));
        } else if (ports.action && ports.action(action, params)) {
            finish({{QStringLiteral("accepted"), true}});
        } else
            error(QStringLiteral("invalid_parameters"), QStringLiteral("action"));
    } else if (method == u"snow_shot_settings_get") {
        finish(settingsSnapshot(params.value(QStringLiteral("section")).toString()));
    } else if (method == u"snow_shot_settings_update") {
        const auto values = params.value(QStringLiteral("values")).toObject();
        QMap<QString, QVariant> drafts;
        QHash<QString, AuxiliaryIntegerSetting> auxiliaries;
        for (const auto& field : ports.settings->registry().fields())
            if (const auto auxiliary = auxiliaryIntegerSetting(field))
                auxiliaries.insert(auxiliary->id, *auxiliary);
        for (auto it = values.begin(); it != values.end(); ++it) {
            const auto* field = ports.settings->registry().field(it.key());
            QVariant value;
            if (const auto auxiliary = auxiliaries.constFind(it.key());
                auxiliary != auxiliaries.cend()) {
                const auto json = it.value();
                if (!json.isDouble() || !std::isfinite(json.toDouble()) ||
                    std::floor(json.toDouble()) != json.toDouble() ||
                    !storage::ConfigurationSchema::normalize(auxiliary->configurationKey, json)
                         .valid) {
                    error(QStringLiteral("invalid_parameters"), it.key());
                    return;
                }
                value = json.toInt();
            } else if (!field || !settingsValue(*field, it.value(), &value)) {
                error(QStringLiteral("invalid_parameters"), it.key());
                return;
            }
            drafts.insert(it.key(), value);
        }
        if (drafts.isEmpty()) {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("values"));
            return;
        }
        if (!expected) {
            error(QStringLiteral("revision_required"));
            return;
        }
        // Reserve observation capacity before accepting any runtime mutation. Synchronous
        // patches release the unpublished slot; asynchronous commits retain an owned job.
        const auto settingsJob = ports.jobs->start(request.connectionId, method);
        if (!lifetime)
            return;
        if (settingsJob.isEmpty()) {
            error(QStringLiteral("queue_full"));
            return;
        }
        QStringList applied;
        QMap<QString, quint64> pending;
        QString rejected;
        bool conflict = false;
        const bool accepted = configuration().mutateIfRevision(
            *expected,
            [&] {
                for (auto it = drafts.cbegin(); it != drafts.cend(); ++it) {
                    const auto auxiliary = auxiliaries.constFind(it.key());
                    const bool submitted = auxiliary == auxiliaries.cend()
                                               ? ports.settings->submitDraft(it.key(), it.value())
                                               : ports.settings->applyIntegerValue(
                                                     auxiliary->binding, it.value().toInt());
                    if (!submitted) {
                        rejected = it.key();
                        return false;
                    }
                    const auto state = ports.settings->state(it.key());
                    if (auxiliary == auxiliaries.cend() &&
                        (state.phase == settings::SettingsWritePhase::Failed ||
                         state.phase == settings::SettingsWritePhase::Rejected)) {
                        rejected = it.key();
                        return false;
                    }
                    if (auxiliary == auxiliaries.cend() &&
                        (state.busy || state.phase == settings::SettingsWritePhase::Pending))
                        pending.insert(it.key(), state.revision);
                    else
                        applied.append(it.key());
                }
                return true;
            },
            &conflict);
        auto response =
            accepted
                ? reply(request,
                        {{QStringLiteral("revision"),
                          static_cast<qint64>(configuration().revision())},
                         {QStringLiteral("applied_fields"), QJsonArray::fromStringList(applied)}})
                : failure(request,
                          conflict ? QStringLiteral("stale_revision")
                                   : QStringLiteral("operation_failed"),
                          rejected);
        response.revision = configuration().revision();
        if (!accepted && !applied.isEmpty())
            response.errorDetails.insert(QStringLiteral("applied_fields"),
                                         QJsonArray::fromStringList(applied));
        observeSettings(request, settingsJob, pending, applied, response);
        if (!lifetime)
            return;
        completion(std::move(response));
    } else if (method == u"snow_shot_settings_reset") {
        const auto* section = ports.settings->registry().catalog().section(
            params.value(QStringLiteral("page_id")).toString(),
            params.value(QStringLiteral("section_id")).toString());
        if (!section || section->reset == settings::SettingsSectionReset::None) {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("section_id"));
            return;
        }
        if (!expected) {
            error(QStringLiteral("revision_required"));
            return;
        }
        const auto job = ports.jobs->start(request.connectionId, method);
        if (!lifetime)
            return;
        if (job.isEmpty()) {
            error(QStringLiteral("queue_full"));
            return;
        }
        QMap<QString, quint64> pending;
        QStringList applied;
        bool conflict = false;
        const bool accepted = configuration().mutateIfRevision(
            *expected, [&] { return ports.settings->reset(section->reset); }, &conflict);
        if (!conflict) {
            for (const auto index : ports.settings->registry().fieldsForReset(section->reset)) {
                const auto& field = ports.settings->registry().fields().at(index);
                const auto state = ports.settings->state(field.id);
                if (state.busy || state.phase == settings::SettingsWritePhase::Pending)
                    pending.insert(field.id, state.revision);
                else if (state.phase == settings::SettingsWritePhase::Clean)
                    applied.append(field.id);
            }
        }
        auto response = accepted ? reply(request, {{QStringLiteral("applied_fields"),
                                                    QJsonArray::fromStringList(applied)}})
                                 : failure(request, conflict ? QStringLiteral("stale_revision")
                                                             : QStringLiteral("operation_failed"));
        response.revision = configuration().revision();
        observeSettings(request, job, pending, applied, response);
        if (!lifetime)
            return;
        completion(std::move(response));
#if SNOW_SHOT_ENABLE_API_CONFIGURATION
    } else if (method == u"snow_shot_models_list") {
        finish({{QStringLiteral("models"), publicModels(ports.settings->customAiModels())},
                {QStringLiteral("revision"), static_cast<qint64>(configuration().revision())}});
    } else if (method == u"snow_shot_models_update" || method == u"snow_shot_credentials_set") {
        auto models = ports.settings->customAiModels();
        if (method == u"snow_shot_credentials_set") {
            const auto id = params.value(QStringLiteral("provider")).toString();
            auto found = std::find_if(models.begin(), models.end(),
                                      [&](const auto& item) { return item.id == id; });
            if (found == models.end()) {
                error(QStringLiteral("not_found"));
                return;
            }
            found->apiKey = params.value(QStringLiteral("secret")).toString();
        } else {
            auto input = params.value(QStringLiteral("models")).toArray();
            for (qsizetype i = 0; i < input.size(); ++i) {
                auto object = input[i].toObject();
                if (!object.contains(QStringLiteral("api_key"))) {
                    QString secret;
                    for (const auto& model : models)
                        if (model.id == object.value(QStringLiteral("id")).toString() &&
                            model.baseUrl == object.value(QStringLiteral("base_url")).toString())
                            secret = model.apiKey;
                    object.insert(QStringLiteral("api_key"), secret);
                }
                if (!object.contains(QStringLiteral("supports_vision")))
                    object.insert(QStringLiteral("supports_vision"), false);
                input[i] = object;
            }
            bool valid = false;
            models = customAiModelsFromJson(input, &valid);
            if (!valid) {
                error(QStringLiteral("invalid_parameters"), QStringLiteral("models"));
                return;
            }
        }
        change([&] { return ports.settings->applyCustomAiModels(models); });
#endif
    } else if (method == u"snow_shot_storage_status") {
        auto result = storageJson(ports.storage->status());
        result.insert(QStringLiteral("revision"), static_cast<qint64>(configuration().revision()));
        finish(result);
    } else if (method == u"snow_shot_permissions_get") {
        if (ports.permissions)
            ports.permissions->refreshNow();
        finish(permissionJson(ports.permissions));
    } else if (method == u"snow_shot_permissions_request") {
        if (!ports.permissions) {
            error(QStringLiteral("unsupported"));
            return;
        }
        for (int i = 0; i < 4; ++i) {
            const auto permission = static_cast<presentation::AppPermission>(i);
            if (presentation::appPermissionId(permission) !=
                params.value(QStringLiteral("permission")).toString())
                continue;
            if (params.value(QStringLiteral("open_settings")).toBool()) {
                ports.permissions->refreshNow();
                if (!ports.permissions->openSettings(permission)) {
                    error(QStringLiteral("unsupported"));
                    return;
                }
            } else
                ports.permissions->request(permission);
            finish(permissionJson(ports.permissions));
            return;
        }
        error(QStringLiteral("invalid_parameters"), QStringLiteral("permission"));
    } else if (method == u"snow_shot_updates_status") {
        finish(updateJson(ports.updates));
    } else if (method == u"snow_shot_updates_action") {
        if (!ports.updates || ports.updates->managedByStore()) {
            error(QStringLiteral("unsupported"));
            return;
        }
        const auto action = params.value(QStringLiteral("action")).toString();
        if (action == u"check" || action == u"download") {
#ifdef Q_OS_MACOS
            if (action == u"download") {
                error(QStringLiteral("unsupported"));
                return;
            }
#endif
            if (ports.updates->busy() || !activeUpdateJob.isEmpty()) {
                error(QStringLiteral("busy"));
                return;
            }
            if (action == u"download" &&
                ports.updates->status().state != update::UpdateState::Available) {
                error(QStringLiteral("invalid_state"));
                return;
            }
            const auto id = ports.jobs->start(request.connectionId, method);
            if (!lifetime)
                return;
            if (id.isEmpty()) {
                error(QStringLiteral("queue_full"));
                return;
            }
            activeUpdateJob = id;
            activeUpdateOwner = request.connectionId;
            auto* observation = new QObject(&q);
            ports.jobs->setCancellation(id, [guard = QPointer<McpApplicationService>(&q),
                                             observation = QPointer<QObject>(observation), id] {
                if (guard && guard->m_impl->activeUpdateJob == id) {
                    guard->m_impl->activeUpdateJob.clear();
                    guard->m_impl->ports.updates->cancel();
                }
                if (observation)
                    observation->deleteLater();
            });
            if (!lifetime)
                return;
            QObject::connect(ports.updates, &update::UpdateService::statusChanged, observation,
                             [this, id] {
                                 if (activeUpdateJob != id)
                                     return;
                                 ports.jobs->progress(id, updateJson(ports.updates));
                             });
            QObject::connect(ports.updates, &update::UpdateService::operationFinished, observation,
                             [this, observation = QPointer<QObject>(observation), id,
                              action](const QString& operation, const QString& outcome) {
                                 if (activeUpdateJob != id || operation != action)
                                     return;
                                 activeUpdateJob.clear();
                                 if (outcome == u"success")
                                     ports.jobs->complete(id, updateJson(ports.updates));
                                 else if (outcome == u"cancelled")
                                     ports.jobs->cancel(activeUpdateOwner, id);
                                 else
                                     ports.jobs->fail(id, QStringLiteral("update_failed"));
                                 if (observation)
                                     observation->deleteLater();
                             });
            finish({{QStringLiteral("job_id"), id}});
            if (!lifetime)
                return;
            if (action == u"check")
                ports.updates->check();
            else
                ports.updates->download();
            return;
        } else if (action == u"cancel") {
            if (!activeUpdateJob.isEmpty() && activeUpdateOwner != request.connectionId) {
                error(QStringLiteral("job_not_found"));
                return;
            }
            if (!activeUpdateJob.isEmpty()) {
                const auto id = activeUpdateJob;
                const auto owner = activeUpdateOwner;
                ports.jobs->cancel(owner, id);
            } else
                ports.updates->cancel();
            if (!lifetime)
                return;
        } else if (action == u"apply") {
            if (ports.updates->status().state != update::UpdateState::Ready) {
                error(QStringLiteral("invalid_state"));
                return;
            }
            if (!ports.restartAllowed || !ports.restartAllowed()) {
                error(QStringLiteral("busy"));
                return;
            }
            auto response = reply(request, {{QStringLiteral("accepted"), true}});
            response.afterSend = [service = QPointer<update::UpdateService>(ports.updates)] {
                if (service)
                    service->beginApply();
            };
            completion(std::move(response));
            return;
        } else {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("action"));
            return;
        }
        finish(updateJson(ports.updates));
    } else if (method == u"snow_shot_history_list") {
        const auto snapshot = ports.storage->captureHistory().recordsSnapshot();
        const auto cursor = params.value(QStringLiteral("cursor")).toString();
        qsizetype offset = 0;
        if (!cursor.isEmpty()) {
            const auto parts = cursor.split(u':');
            bool revisionOk = false, offsetOk = false;
            const auto revision = parts.value(0).toULongLong(&revisionOk);
            offset = parts.value(1).toLongLong(&offsetOk);
            if (parts.size() != 2 || !revisionOk || !offsetOk || revision != snapshot.revision ||
                offset < 0 || offset > snapshot.records.size()) {
                error(QStringLiteral("invalid_cursor"));
                return;
            }
        }
        const int limit = params.value(QStringLiteral("limit")).toInt(50);
        if (limit < 1 || limit > 200) {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("limit"));
            return;
        }
        const auto query = params.value(QStringLiteral("query")).toString();
        QJsonArray records;
        qsizetype next = offset;
        for (; next < snapshot.records.size() && records.size() < limit; ++next) {
            const auto& record = snapshot.records[next];
            if (query.isEmpty() || record.id.contains(query, Qt::CaseInsensitive) ||
                record.createdUtc.toString(Qt::ISODate).contains(query))
                records.append(historyJson(record));
        }
        finish({{QStringLiteral("records"), records},
                {QStringLiteral("revision"), static_cast<qint64>(snapshot.revision)},
                {QStringLiteral("next_cursor"),
                 next < snapshot.records.size()
                     ? QString::number(snapshot.revision) + u':' + QString::number(next)
                     : QString()}});
    } else if (method == u"snow_shot_history_get" || method == u"snow_shot_history_action") {
        const auto snapshot = ports.storage->captureHistory().recordsSnapshot();
        const auto id = params.value(QStringLiteral("history_id")).toString();
        const auto it = std::find_if(snapshot.records.cbegin(), snapshot.records.cend(),
                                     [&](const auto& record) { return record.id == id; });
        if (it == snapshot.records.cend()) {
            error(QStringLiteral("not_found"));
            return;
        }
        if (method == u"snow_shot_history_action") {
            if (!ports.historyAction ||
                !ports.historyAction(id, params.value(QStringLiteral("action")).toString())) {
                error(QStringLiteral("busy"));
                return;
            }
            finish({{QStringLiteral("accepted"), true}});
            return;
        }
        auto metadata = historyJson(*it);
        metadata.insert(QStringLiteral("revision"), static_cast<qint64>(snapshot.revision));
        if (!params.value(QStringLiteral("include_image")).toBool()) {
            finish(metadata);
            return;
        }
        auto* repository = &ports.storage->captureHistory();
        background(
            request,
            [repository, record = *it, request, metadata] {
                const auto png = repository->loadResultPng(record);
                if (!png)
                    return failure(request, QStringLiteral("image_unavailable"));
                // Header validation alone cannot detect damaged IDAT data in a stored PNG.
                // Validate pixels before returning the existing encoding, as the previous
                // decode-and-encode path did, without performing a second PNG encode.
                QByteArray recompressed;
                bool encodeFailed = false;
                const auto valid = [&] {
                    const QImage image = image_codec::decode(png->bytes(), snow::image::Format::png,
                                                             "capture_result.png");
                    if (image.isNull() || image.size() != png->pixelSize())
                        return false;
                    if (png->bytes().size() > 60 * 1024 * 1024) {
                        // A low-compression stored PNG may exceed the response limit even
                        // though the previous default encoding fit. Keep that fallback.
                        QBuffer buffer(&recompressed);
                        encodeFailed =
                            !buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG");
                    }
                    return true;
                }();
                if (!valid) {
                    repository->reportReadFailure(record, QStringLiteral("image_unavailable"));
                    return failure(request, QStringLiteral("image_unavailable"));
                }
                if (encodeFailed)
                    return failure(request, QStringLiteral("output_failed"));
                auto response = reply(request, metadata);
                response.attachment = recompressed.isEmpty() ? png->bytes() : recompressed;
                if (response.attachment.size() > 60 * 1024 * 1024)
                    return failure(request, QStringLiteral("output_too_large"));
                response.attachmentMime = QStringLiteral("image/png");
                return response;
            },
            [this, guard = QPointer<McpApplicationService>(&q), request,
             completion = std::move(completion)](auto response) mutable {
                if (response.ok && !response.attachment.isEmpty()) {
                    const auto writer = ports.artifactWriter;
                    const auto artifact =
                        writer ? writer(request.connectionId, std::move(response.attachment),
                                        response.attachmentMime)
                               : QJsonObject();
                    if (!guard)
                        return;
                    if (artifact.isEmpty())
                        response = failure(request, QStringLiteral("capacity_exceeded"));
                    else {
                        response.attachment.clear();
                        response.attachmentMime.clear();
                        response.result.insert(QStringLiteral("artifact"), artifact);
                    }
                }
                completion(std::move(response));
            });
    } else if (method == u"snow_shot_history_delete" || method == u"snow_shot_history_clear") {
        if (!expected) {
            error(QStringLiteral("revision_required"));
            return;
        }
        storageJob(
            request,
            [&] {
                return ports.storage->captureHistory().removeIfRevision(
                    {params.value(QStringLiteral("history_id")).toString()}, *expected,
                    method == u"snow_shot_history_clear");
            },
            std::move(completion));
    } else if (method == u"snow_shot_configuration_export") {
        QString path;
        if (!ScreenshotMcpSession::validateOutputPath(
                params.value(QStringLiteral("path")).toString(), &path)) {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("path"));
            return;
        }
        const auto values = configuration().snapshot();
        background(
            request,
            [request, path, values] {
                const auto result = storage::ConfigurationArchive::write(
                    path, values, storage::ConfigurationStore::currentSchemaVersion(), true);
                if (!result.isEmpty())
                    return failure(request, QStringLiteral("output_failed"));
                return reply(request, {{QStringLiteral("path"), path},
                                       {QStringLiteral("credentials_redacted"), true}});
            },
            std::move(completion), false);
    } else if (method == u"snow_shot_configuration_import") {
        const QFileInfo file(params.value(QStringLiteral("path")).toString());
        if (!file.isAbsolute() || !file.isFile() || file.isSymLink() || !expected) {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("path"));
            return;
        }
        if (admittedWork >= 2) {
            error(QStringLiteral("queue_full"));
            return;
        }
        auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto id =
            ports.jobs->start(request.connectionId, method, [cancel] { cancel->store(true); });
        if (!lifetime)
            return;
        if (id.isEmpty()) {
            error(QStringLiteral("queue_full"));
            return;
        }
        ++admittedWork;
        auto* watcher = new QFutureWatcher<storage::ConfigurationArchiveReadResult>(&q);
        QObject::connect(
            watcher, &QFutureWatcher<storage::ConfigurationArchiveReadResult>::finished, &q,
            [this, guard = QPointer<McpApplicationService>(&q), watcher, cancel, id,
             revision = *expected] {
                auto read = watcher->result();
                watcher->deleteLater();
                --admittedWork;
                if (cancel->load() || stopping)
                    return;
                if (!read.isValid()) {
                    ports.jobs->fail(id, QStringLiteral("invalid_archive"));
                    return;
                }
                if (!ports.jobs->setCancellation(id, {}))
                    return;
                if (!guard)
                    return;
                bool conflict = false;
                std::shared_future<storage::StorageResult> runtimeCompletion;
                const bool ok = configuration().mutateIfRevision(
                    revision,
                    [&] {
                        const auto current = configuration().snapshot();
                        read.preserveOmittedDirectories(current);
                        read.preserveOmittedCredentials(current);
                        return ports.settings->importConfigurationSnapshot(
                            read.values, read.schemaVersion, &runtimeCompletion);
                    },
                    &conflict);
                if (!guard)
                    return;
                if (runtimeCompletion.valid()) {
                    ++admittedWork;
                    auto* runtimeWatcher = new QFutureWatcher<storage::StorageResult>(&q);
                    QObject::connect(
                        runtimeWatcher, &QFutureWatcher<storage::StorageResult>::finished, &q,
                        [this, runtimeWatcher, id, ok] {
                            const auto result = runtimeWatcher->result();
                            runtimeWatcher->deleteLater();
                            --admittedWork;
                            if (stopping)
                                return;
                            if (ok && result.success)
                                ports.jobs->complete(
                                    id, {{QStringLiteral("revision"),
                                          static_cast<qint64>(configuration().revision())}});
                            else
                                ports.jobs->fail(id, QStringLiteral("storage_failed"));
                        });
                    runtimeWatcher->setFuture(runMcpWork(
                        &workers, [runtimeCompletion] { return runtimeCompletion.get(); }));
                } else if (ok)
                    ports.jobs->complete(id, {{QStringLiteral("revision"),
                                               static_cast<qint64>(configuration().revision())}});
                else
                    ports.jobs->fail(id, conflict ? QStringLiteral("stale_revision")
                                                  : QStringLiteral("storage_failed"));
            });
        watcher->setFuture(runMcpWork(&workers, [path = file.absoluteFilePath()] {
            return storage::ConfigurationArchive::read(path);
        }));
        finish({{QStringLiteral("job_id"), id}});
    } else if (method == u"snow_shot_settings_action" || method == u"snow_shot_storage_cleanup") {
        settings::SettingsActionBinding binding;
        if (method == u"snow_shot_settings_action") {
            const auto* field = ports.settings->registry().field(
                params.value(QStringLiteral("field_id")).toString());
            const auto* action =
                field && field->definition
                    ? std::get_if<settings::SettingsActionDefinition>(&field->definition->payload)
                    : nullptr;
            if (!action) {
                error(QStringLiteral("invalid_parameters"), QStringLiteral("field_id"));
                return;
            }
            binding = action->binding;
        } else {
            const auto kind = params.value(QStringLiteral("kind")).toString();
            if (kind == u"history")
                binding = settings::SettingsActionBinding::ClearCaptureHistory;
            else if (kind == u"pinned")
                binding = settings::SettingsActionBinding::ClearPinnedHistory;
            else if (kind == u"thumbnail_cache")
                binding = settings::SettingsActionBinding::ClearThumbnailCache;
            else if (kind == u"temporary_files")
                binding = settings::SettingsActionBinding::ClearRecordingTemp;
            else {
                error(QStringLiteral("invalid_parameters"), QStringLiteral("kind"));
                return;
            }
        }
        if (binding == settings::SettingsActionBinding::ExportConfiguration ||
            binding == settings::SettingsActionBinding::ImportConfiguration) {
            auto forwarded = request;
            forwarded.method = binding == settings::SettingsActionBinding::ExportConfiguration
                                   ? QStringLiteral("snow_shot_configuration_export")
                                   : QStringLiteral("snow_shot_configuration_import");
            handle(forwarded, std::move(completion));
            return;
        }
        if (!expected) {
            error(QStringLiteral("revision_required"));
            return;
        }
        if (configuration().revision() != *expected) {
            error(QStringLiteral("stale_revision"));
            return;
        }
        const auto actionState = ports.settings->actionState(binding);
        if (!actionState.enabled || actionState.busy) {
            error(QStringLiteral("busy"));
            return;
        }
        if (binding == settings::SettingsActionBinding::RestartAsAdministrator) {
            if (!ports.restartAllowed || !ports.restartAllowed()) {
                error(QStringLiteral("busy"));
                return;
            }
            auto response = reply(request, {{QStringLiteral("accepted"), true}});
            response.afterSend = [guard = QPointer<McpApplicationService>(&q), binding,
                                  revision = *expected] {
                if (guard)
                    guard->m_impl->configuration().mutateIfRevision(revision, [&] {
                        return guard->m_impl->ports.settings->triggerAction(binding);
                    });
            };
            completion(std::move(response));
            return;
        }
        if (binding == settings::SettingsActionBinding::OpenLoginItemSettings) {
            change([&] { return ports.settings->triggerAction(binding); });
            return;
        }
        const auto id = ports.jobs->start(request.connectionId, method);
        if (!lifetime)
            return;
        if (id.isEmpty()) {
            error(QStringLiteral("queue_full"));
            return;
        }
        auto* observation = new QObject(&q);
        const bool cache = binding == settings::SettingsActionBinding::ClearThumbnailCache ||
                           binding == settings::SettingsActionBinding::ClearRecordingTemp;
        const auto observe = [this, observation = QPointer<QObject>(observation), id, binding] {
            if (ports.settings->actionState(binding).busy)
                return;
            const auto status = ports.storage->status();
            const bool failed = binding == settings::SettingsActionBinding::ClearCaptureHistory
                                    ? !status.lastHistoryError.isEmpty()
                                : binding == settings::SettingsActionBinding::ClearPinnedHistory
                                    ? !status.lastPinnedError.isEmpty()
                                    : false;
            if (failed)
                ports.jobs->fail(id, QStringLiteral("storage_failed"));
            else
                ports.jobs->complete(id, {{QStringLiteral("completed"), true}});
            if (observation)
                observation->deleteLater();
        };
        if (binding == settings::SettingsActionBinding::CopyTodayLog) {
            QObject::connect(
                ports.settings, &settings::SettingsRuntimeSession::actionFinished, observation,
                [this, observation = QPointer<QObject>(observation), id,
                 binding](auto action, bool success, const QString&) {
                    if (action != binding)
                        return;
                    if (success)
                        ports.jobs->complete(id, {{QStringLiteral("completed"), true}});
                    else
                        ports.jobs->fail(id, QStringLiteral("output_failed"));
                    if (observation)
                        observation->deleteLater();
                });
        } else if (cache) {
            QObject::connect(
                ports.storage, &storage::ApplicationStorage::cacheClearFinished, observation,
                [this, observation = QPointer<QObject>(observation), id](auto, bool success) {
                    if (success)
                        ports.jobs->complete(id, {{QStringLiteral("completed"), true}});
                    else
                        ports.jobs->fail(id, QStringLiteral("storage_failed"));
                    if (observation)
                        observation->deleteLater();
                });
        } else {
            QObject::connect(ports.storage, &storage::ApplicationStorage::storageStatusChanged,
                             observation, [observe](const auto&) { observe(); });
        }
        bool conflict = false;
        const bool accepted = configuration().mutateIfRevision(
            *expected,
            [&] {
                return ports.settings->triggerAction(
                    binding, params.value(QStringLiteral("path")).toString());
            },
            &conflict);
        if (!lifetime)
            return;
        if (!accepted) {
            delete observation;
            ports.jobs->fail(id, conflict ? QStringLiteral("stale_revision")
                                          : QStringLiteral("operation_failed"));
            if (!lifetime)
                return;
            error(conflict ? QStringLiteral("stale_revision") : QStringLiteral("operation_failed"));
            return;
        }
        finish({{QStringLiteral("job_id"), id},
                {QStringLiteral("revision"), static_cast<qint64>(configuration().revision())}});
        if (!lifetime)
            return;
        if (binding != settings::SettingsActionBinding::CopyTodayLog && !cache)
            observe();
    } else if (method == u"snow_shot_templates_list") {
        const auto kind = params.value(QStringLiteral("kind")).toString();
        QJsonArray items;
        if (kind == u"drawing") {
            for (const auto& item : storage::DrawTemplateSettings().templates())
                items.append(QJsonObject{
                    {QStringLiteral("name"), item.name},
                    {QStringLiteral("payload"), QJsonDocument::fromJson(item.payload).object()}});
        } else if (kind == u"watermark") {
            for (const auto& item : storage::WatermarkTemplateSettings().templates())
                items.append(QJsonObject{{QStringLiteral("name"), item.name},
                                         {QStringLiteral("text"), item.value}});
        } else {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("kind"));
            return;
        }
        finish({{QStringLiteral("templates"), items},
                {QStringLiteral("revision"), static_cast<qint64>(configuration().revision())}});
    } else if (method == u"snow_shot_templates_update") {
        const auto kind = params.value(QStringLiteral("kind")).toString();
        const auto items = params.value(QStringLiteral("templates")).toArray();
        if (items.size() > 128) {
            error(QStringLiteral("invalid_parameters"));
            return;
        }
        QVector<storage::DrawTemplate> drawings;
        QVector<storage::WatermarkTemplate> watermarks;
        QSet<QString> names;
        for (const auto& value : items) {
            const auto item = value.toObject();
            const auto name = item.value(QStringLiteral("name")).toString().trimmed();
            if (name.isEmpty() || names.contains(name) || name.size() > 128) {
                error(QStringLiteral("invalid_parameters"), QStringLiteral("templates"));
                return;
            }
            names.insert(name);
            if (kind == u"drawing" && item.value(QStringLiteral("payload")).isObject())
                drawings.append(
                    {name, QJsonDocument(item.value(QStringLiteral("payload")).toObject())
                               .toJson(QJsonDocument::Compact)});
            else if (kind == u"watermark" && item.value(QStringLiteral("text")).isString())
                watermarks.append({name, item.value(QStringLiteral("text")).toString()});
            else {
                error(QStringLiteral("invalid_parameters"), QStringLiteral("templates"));
                return;
            }
        }
        if (kind != u"drawing" && kind != u"watermark") {
            error(QStringLiteral("invalid_parameters"));
            return;
        }
        change([&] {
            return kind == u"drawing"
                       ? storage::DrawTemplateSettings().setTemplates(drawings)
                       : storage::WatermarkTemplateSettings().setTemplates(watermarks);
        });
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    } else if (method == u"snow_shot_translation_catalog") {
        if (!ports.translation) {
            error(QStringLiteral("unavailable"));
            return;
        }
        ports.translation->refreshModels();
        QJsonArray languages, models;
        for (const auto& language : translation::translationLanguages())
            languages.append(QJsonObject{
                {QStringLiteral("code"), QString::fromUtf8(language.code)},
                {QStringLiteral("name"),
                 translation::translationLanguageName(QString::fromUtf8(language.code))}});
        for (const auto& model : ports.translation->models())
            models.append(QJsonObject{{QStringLiteral("id"), model.id},
                                      {QStringLiteral("name"), model.name}});
        const auto preferences = ports.translation->preferences();
        finish({{QStringLiteral("models"), models},
                {QStringLiteral("languages"), languages},
                {QStringLiteral("loading"), ports.translation->loadingModels()},
                {QStringLiteral("preferences"),
                 QJsonObject{{QStringLiteral("source_language"), preferences.sourceLanguage},
                             {QStringLiteral("target_language"), preferences.targetLanguage},
                             {QStringLiteral("model_id"), preferences.modelId}}}});
    } else if (method == u"snow_shot_translation_start") {
        if (!ports.translation || !ports.jobs) {
            error(QStringLiteral("unavailable"));
            return;
        }
        auto input = params;
        if (params.contains(QStringLiteral("retry_job_id"))) {
            for (const auto& key :
                 {QStringLiteral("source"), QStringLiteral("texts"),
                  QStringLiteral("source_language"), QStringLiteral("target_language"),
                  QStringLiteral("model_id")}) {
                if (params.contains(key)) {
                    error(QStringLiteral("invalid_parameters"), key);
                    return;
                }
            }
            const auto previousId = params.value(QStringLiteral("retry_job_id")).toString();
            const auto previous = ports.jobs->get(request.connectionId, previousId);
            if (!previous || previous->value(QStringLiteral("kind")) != method) {
                error(QStringLiteral("job_not_found"));
                return;
            }
            if (previous->value(QStringLiteral("status")) == QStringLiteral("running")) {
                error(QStringLiteral("invalid_state"));
                return;
            }
            const auto retained = ports.jobs->input(request.connectionId, previousId);
            if (!retained) {
                error(QStringLiteral("retry_unavailable"));
                return;
            }
            input = *retained;
        }
        QStringList texts;
        const bool selected = input.value(QStringLiteral("source")).toString() == u"selection";
        if ((!selected && (!stringArray(input.value(QStringLiteral("texts")), &texts) ||
                           texts.isEmpty() || texts.size() > 256)) ||
            (selected && input.contains(QStringLiteral("texts")))) {
            error(QStringLiteral("invalid_parameters"), QStringLiteral("texts"));
            return;
        }
        if (selected && !ports.selectedText) {
            error(QStringLiteral("unsupported"));
            return;
        }
        auto preferences = ports.translation->preferences();
        if (input.contains(QStringLiteral("source_language")))
            preferences.sourceLanguage = input.value(QStringLiteral("source_language")).toString();
        if (input.contains(QStringLiteral("target_language")))
            preferences.targetLanguage = input.value(QStringLiteral("target_language")).toString();
        if (params.contains(QStringLiteral("retry_job_id"))) {
            preferences.secondaryTargetLanguage =
                input.value(QStringLiteral("secondary_target_language")).toString();
            preferences.languageDetectionConfidenceThreshold =
                input.value(QStringLiteral("language_detection_confidence_threshold")).toInt(60);
        }
        if (input.contains(QStringLiteral("model_id")))
            preferences.modelId = input.value(QStringLiteral("model_id")).toString();
        auto canceled = std::make_shared<bool>(false);
        auto captureCancellation = std::make_shared<std::function<void()>>();
        const auto id =
            ports.jobs->start(request.connectionId, method, [canceled, captureCancellation] {
                *canceled = true;
                if (*captureCancellation)
                    (*captureCancellation)();
            });
        if (!lifetime)
            return;
        if (id.isEmpty()) {
            error(QStringLiteral("queue_full"));
            return;
        }
        finish({{QStringLiteral("job_id"), id}});
        if (!lifetime)
            return;
        if (selected) {
            const auto selectedText = ports.selectedText;
            *captureCancellation = selectedText([guard = QPointer<McpApplicationService>(&q), id,
                                                 canceled,
                                                 preferences](QString text, QString captureError) {
                if (!guard || *canceled)
                    return;
                auto& self = *guard->m_impl;
                if (!captureError.isEmpty() || text.isEmpty()) {
                    self.ports.jobs->fail(
                        id, captureError.isEmpty() ? QStringLiteral("no_selection") : captureError);
                    return;
                }
                self.translate(id, {text}, preferences);
            });
        } else
            translate(id, texts, preferences);
#endif
    } else {
        error(QStringLiteral("method_not_found"));
    }
}
} // namespace snow_shot::app::mcp

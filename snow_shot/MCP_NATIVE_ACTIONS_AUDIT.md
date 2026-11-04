# Native action / external interface audit

Baseline: upstream `4234e42cde1b56549e34ea04d0edbf426db0c1d2` (main, 2026-11-04).
This is newer than the 1.4.5 release used by the supplied requirements. This audit
records baseline source evidence and scoped validation below. No OrbitPie-specific transport,
client identity, executable path, or workflow is part of the interface design.

## Native entry points

Authority: `GlobalShortcutAction`, `ApplicationController::dispatchAvailableQuickAction`,
`ScreenshotController::CaptureAction`, Qt MCP adapters, and Rust input schemas.
Tool names below omit the `snow_shot_` prefix. This table describes baseline gaps;
implemented changes and validation are tracked below.

| Native action | Existing interface / parameters | Conditions / gap at baseline |
| --- | --- | --- |
| Screenshot | `screenshot_begin` | Always preconfirms target; missing interactive selection |
| ScreenshotDelay | `document_open(source=capture, delay_seconds)` | Background capture only; not native delayed selection |
| ScreenshotFixed | `screenshot_pin` | Requires owned confirmed selection; no select-then-pin entry |
| ScreenshotOcr | `screenshot_recognize(kind=text)` | Requires owned selection and configured OCR |
| ScreenshotOcrCopy | recognize + `screenshot_export_recognition(output=copy)` | Requires selection; no native selection workflow entry |
| ScreenshotTranslation | `screenshot_translate` | Full edition, selection, configured provider |
| ScreenshotCopy | `screenshot_copy` | Requires owned confirmed selection |
| ScreenshotSave | `screenshot_save(path/automatic_path, format)` | File export, not native Save dialog after selection |
| ScreenshotQuickSave | `screenshot_save(automatic_path=true)` | Requires owned confirmed selection |
| ScreenshotFullScreen | `screenshot_direct_capture(target=current_monitor)` | Resolves current monitor; render/save/copy |
| ScreenshotFocusedWindow | `screenshot_direct_capture(target=focused_window)` | Resolves focus at invocation; no explicit window identity |
| ScreenRecord | `recording_start(region, options)` | Explicit rectangle only; missing interactive selection |
| ScreenRecordCopy | `recording_control` | Requires owned recording; no select/start/stop-copy workflow |
| OpenScreenRecordingFolder | No dedicated app action | Missing native window/folder action |
| OpenCaptureHistory | `app_action(action=show_history)` | Opens native UI; accepted is not user workflow completion |
| OpenSettings | `app_action(action=show_settings, page_id, section_id)` | Registered settings location |
| OpenPinToScreenManagement | `app_action(action=show_pinned)` | Native management window |
| PinClipboardContent | `pinned_create` / document clipboard source | Content/type and actual creation must be checked |
| PinSelectedFiles | No explicit selected-file entry | Foreground target must be captured before launcher changes focus |
| RestoreLastClosedWindows | `pinned_action(action=restore_last)` | Requires current group revision; acceptance is not pin creation completion |
| HideShowAllWindows | `pinned_action` / groups | Per-object operations are not global native toggle |
| SwitchWindowGroup | Group tools | Data operations are not the native picker |
| GlobalCanvas | No direct entry | Missing native action |
| StitchSelectedImages | No direct selected-file entry | Requires explicit source selection |
| SplitSelectedImages | No direct selected-file entry | Requires explicit source selection |
| TranslateSelectedText | `translation_start(source=selection)` | Full edition, provider; foreground target timing matters |
| ToggleWindowUnderCursorAlwaysOnTop | No direct entry | Windows; explicit window identity needed |
| ToggleGlobalHotkeys | Settings tools | Administrative action; separate from ordinary capture |
| ToggleDisableOnFocusedFullscreenWindow | Settings tools | Administrative setting; preserve unrelated values |

Additional selection actions exist for scrolling, table/QR/formula recognition and
Markdown/HTML conversion. Their explicit MCP operations must remain available;
the native action list is not a closed list of all application capabilities.

## Cross-cutting findings

- Existing screenshot ownership is connection-scoped; visible work survives
  disconnect as user-owned work. Silent work is canceled. Preserve this behavior.
- Begin waits for `mcpCapturePresented`, with a 60-second pending timeout. The
  controller's presented callback currently confirms the entire requested target.
- Screenshot state includes physical/logical/canvas display maps and backend IDs.
  It does not establish that the acquired frame postdates the request.
- Idempotency caches completed responses; an in-flight duplicate currently gets
  busy. Queries/cancellation have a transport control lane, while begin does not.
- Existing OCR/translation operations and jobs are asynchronous. Their completion
  differs from native UI workflow completion; do not infer completion from begin.
- Tools have typed schemas, but the discovery surface lacks a localized native
  action catalog with reusable icons and explicit invocation templates.
- Explicit exclusion identities, fresh-frame guarantees, caller input isolation,
  startup stage timing and terminal UI outcome retention need implementation or
  native evidence. They must not be advertised as supported without it.

## Acceptance tracking

Implemented changes: interactive selection
without implicit confirmation; request lookup and in-flight replay; localized
native action catalog with explicit unsupported entries; independent `client`
and `user` control modes for screenshot begin. User mode releases protocol
ownership after presentation without canceling the native editor. These modes
do not turn client-side catalog recipes into automatic native workflows.
Startup state also reports monotonic dispatch, receive, and presentation offsets;
these diagnostics do not establish native frame freshness.

Validation on Windows x64 with Qt 6.12.0 (2026-11-04):

- Built the Debug application, MCP bridge, and relevant C++ test targets.
- Six focused CTest cases passed: begin lifecycle, control transport, replay
  lifecycle, application adapter, capture workflow, and controller lifetime.
- Rust begin schema tests passed in full and mini configurations; Clippy passed.
  Bridge stdio tests and full/mini capability contract checks passed.
- Isolated production application catalog checks passed, including Chinese
  labels and PNG icons.
- Isolated native Windows capture returned ready with an unconfirmed empty
  selection. Client-owned capture remained queryable and API cancellation
  returned canceled=true and idle. First invocation reported presentation at
  393 ms and RPC completion at 419 ms; these are one observation, not a latency
  guarantee.
- User-owned capture returned handed_off=true, active=false, and no session ID.
  API cancellation returned false. After bridge disconnect/reconnect the native
  capture remained busy according to the application catalog. This checks
  protocol ownership and application state, not visual persistence or Esc.

## Native action completion

The baseline gaps above now have native invocation adapters. Catalog version 2
routes every registered global action through the application's original native
dispatcher, including delayed selection, recording selection/start-stop-copy,
canvas, selected files, native save dialog, visibility, restore and shortcut gates.
Additional toolbar selection workflows expose scrolling, table/barcode/formula,
Markdown/HTML, print, upload, live-window pins and platform-native AirDrop.
The registry and edition/platform availability remain authoritative; action IDs
are discovered dynamically rather than hard-coded in the Rust bridge.

`app_action(action=native, action_id=...)` accepts user-owned native workflows.
Acceptance is not readiness or completed output. Client-controlled screenshot,
document and recording tools remain available independently; existing catalog
recipes are retained under `client_workflow`. Foreground selection is resolved
at invocation, with no claim of remembering a window before a launcher stole focus.
Delayed screenshot accepts a per-call override without changing settings, supports
zero and durations above the settings UI range, and uses a replaceable single-shot
timer bounded only by its millisecond representation.

Validation for native entry completion (Windows x64, Qt 6.12.0):

- Application and bridge rebuilt successfully. Focused application and controller
  lifetime tests passed, covering every advertised native dispatch, replay,
  invalid IDs/parameters, capture conflicts, and the real single-shot timer at
  20 seconds and zero seconds without acquiring a screen in the unit test.
- Native action schema tests passed in full and mini bridges; Clippy, stdio
  contracts, and both capability manifests passed.
- Isolated production IPC discovered 38 callable actions (28 registered global
  actions plus 10 supplemental selection actions on Windows). Opening settings
  remains available through the existing `show_settings` application action.
  Chinese labels, icons, native history dispatch and replay passed.
- A separate native Windows fixture accepted a zero-delay native screenshot;
  subsequent catalog state was capture-busy and a competing client begin was
  rejected as busy. This verifies native dispatch and session coexistence, not
  visual selection or completed screenshot output. Fixtures were terminated
  after checks; the installed application was not replaced.

Pending desktop acceptance: manual selection/Esc and visual overlay checks (the
UI automation surface did not expose the capture overlay), held input, mixed-DPI
monitors, actual print/upload/AirDrop and provider outputs. Fresh-frame/exclusion
guarantees and broader lifecycle recovery from the supplied requirements are
separate capabilities; exposing native entry points does not establish them.

Catalog metadata follow-up: all 38 Windows actions now expose a boolean
`requires_capture`, native icon metadata/optional PNG, localized category label,
and service prerequisites. Supplemental descriptors reuse native toolbar icons.
Unchecked services report null availability instead of assuming success. The
record/copy toggle reports closed, ready and recording behavior separately;
starting selection or recording requires capture, stopping/copying does not.
Focused application tests cover these three states. The rebuilt production
fixture verified PNG payloads and complete metadata for every catalog entry.

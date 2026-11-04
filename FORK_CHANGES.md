# Modified source notice

Modification date: 2026-11-04
Publisher: ShangZhenYu-CR
Upstream: https://github.com/mg-chao/snow-apps
Base commit: 4234e42cde1b56549e34ea04d0edbf426db0c1d2

This is an independent modified source snapshot, not an official upstream release.
Original copyright, license, warranty and third-party notices are retained.
Application and original build-orchestration modifications follow their existing
GPL-3.0-or-later scope, except for nested file/package declarations. In particular,
the MCP bridge package retains its upstream GPL-3.0-only Cargo.toml declaration;
its changes do not change that license. Existing Apache-2.0 components are not relicensed. The three vcpkg overlay port
changes retain the licensing and attribution of their respective upstream material.
The complete license texts, component COPYRIGHT files, third-party notices and
pinned OCR submodule reference are included; no additional restriction is imposed.

## Changes

- Interactive screenshot selection with independent client control or user handoff.
- Native action discovery and invocation through the actual application dispatcher.
- Delayed selection, recording workflows, canvas, selected-file actions, and
  supplemental recognition/export workflows without binding them to one client.
- Native icons, translated categories, service prerequisites and explicit capture
  semantics, including all three recording/copy toggle states.
- Request lookup/replay and monotonic capture startup diagnostics.
- Focused tests, interface documentation and validation records.
- MSVC CRT compatibility corrections in the Crashpad, minizip-ng and OpenCV ports
  to honor the selected triplet when building with shared Qt.
- Configurable distribution version and Windows x64 portable Release packaging.
- Read-only update probes; an opt-out portable copy no longer removes an installed
  copy's startup or shell registration during normal startup. Explicit Settings
  changes retain their original behavior.

## Validation and distribution scope

Windows x64 Debug application and Rust MCP bridge built with Qt 6.12.0.
Relevant C++ tests, Rust schema checks, Clippy, bridge stdio and capability checks
passed. Production isolated IPC verified all 38 Windows action metadata records,
PNG icons, dispatch and replay. Recording toggle states have deterministic tests.
See snow_shot/MCP_NATIVE_ACTIONS_AUDIT.md for precise evidence and remaining
native desktop acceptance. Printing, cloud upload and platform-specific final
outputs are not all end-to-end verified. No full test suite was run.

The Windows x64 Release portable build also passed distribution and offscreen
update probes with developer directories removed from PATH. Its packaged MCP
bridge verified all 38 action records and PNG icons, interactive screenshot
begin/state/cancel, and normal application quit. The packaged OCR worker reports
version 1.2.1 / protocol 7. Startup and shell-registration registry snapshots
remained unchanged during these checks. This is a same-machine deployment check,
not a claim of testing on every clean Windows installation.

The source snapshot excludes local SDKs, credentials, build outputs, screenshots
and logs. Runnable portable software and corresponding source archives are separate
release assets. The existing upstream build scripts and license collection scripts
remain available. The unrelated local deletion of
scripts/initialize-snow-shot-arm64-libclang.ps1 is not part of this distribution;
its upstream version is retained in the published snapshot.

## Added or modified files

- `FORK_CHANGES.md`
- `README.md`
- `CMakeLists.txt`
- `docs-portable-mcp-build.md`
- `scripts/test-snow-shot-portable.py`
- `snow_shot/src/app/main.cpp`
- `cmake/vcpkg-overlay-ports/crashpad/portfile.cmake`
- `cmake/vcpkg-overlay-ports/minizip-ng/portfile.cmake`
- `cmake/vcpkg-overlay-ports/opencv4/portfile.cmake`
- `snow_shot/CMakeLists.txt`
- `snow_shot/MCP.md`
- `snow_shot/MCP_NATIVE_ACTIONS_AUDIT.md`
- `snow_shot/include/snow_shot/app/mcp/mcpapplicationservice.h`
- `snow_shot/include/snow_shot/app/mcp/mcpnativeactions.h`
- `snow_shot/include/snow_shot/presentation/screenshotcontroller.h`
- `snow_shot/include/snow_shot/presentation/screenshotstartupcontext.h`
- `snow_shot/mcp-capabilities-mini.json`
- `snow_shot/mcp-capabilities.json`
- `snow_shot/mcp-capabilities.source.json`
- `snow_shot/rust/snow-shot-mcp/src/app_client.rs`
- `snow_shot/rust/snow-shot-mcp/src/domain_schemas.rs`
- `snow_shot/rust/snow-shot-mcp/src/schemas.rs`
- `snow_shot/rust/snow-shot-mcp/src/server.rs`
- `snow_shot/src/app/applicationcontroller.cpp`
- `snow_shot/src/app/mcp/mcpapplicationservice.cpp`
- `snow_shot/src/app/mcp/screenshotmcpserver.cpp`
- `snow_shot/src/app/mcp/screenshotmcpsession.cpp`
- `snow_shot/src/presentation/capture/screenshotcaptureworkflow.cpp`
- `snow_shot/src/presentation/services/screenshotcontroller.cpp`
- `snow_shot/src/presentation/services/screenshotmcpcommands_p.h`
- `snow_shot/tests/check_mcp_capabilities.py`
- `snow_shot/tests/mcp_application_fixture_tests.py`
- `snow_shot/tests/mcp_application_tests.cpp`
- `snow_shot/tests/mcp_contract_fixtures.json`
- `snow_shot/tests/mcp_fixture_tests.py`
- `snow_shot/tests/mcp_live_tests.py`
- `snow_shot/tests/mcp_stdio_tests.py`
- `snow_shot/tests/screenshot_capture_workflow_tests.cpp`
- `snow_shot/tests/screenshot_controller_lifetime_tests.cpp`
- `snow_shot/tests/screenshot_controller_test_access.h`
- `snow_shot/tests/screenshot_mcp_tests.cpp`

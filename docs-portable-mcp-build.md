# Snow Shot MCP portable Windows build

Modified distribution, 2026-11-04. See `FORK_CHANGES.md` for provenance and licenses.

The first preview is a Windows x64 Release build using shared Qt 6.12.0 and
MSVC 14.51. Its software download is a `.7z` archive. Extract the entire archive
to a writable directory and run `Start Snow Shot.cmd`; the application is
`bin/snow_shot.exe` and the stdio MCP bridge is `bin/snow-shot-mcp.exe`.
Settings and history stay in the adjacent `data` directory, selected by
`bin/__data_directory`. The package enables MCP, disables automatic updates,
and leaves startup/context-menu registration off by default. These are editable
defaults, not protocol restrictions. Close another Snow Shot instance before
launching this copy because the normal single-instance identity is shared.

Use this repository's Releases to update this modified build. The original
application's updater still uses the upstream release service; manually enabling
it can replace this build with upstream Snow Shot.

## Build

Provision the tools documented in `AGENTS.md`, the shared Qt kit under
`.tools/qt/6.12.0/msvc2022_64`, dynamic vcpkg dependencies, and libclang under
`.tools/llvm-x64/bin`. Initialize the pinned OCR submodule. In an x64 MSVC
developer PowerShell with CMake and Rust on PATH:

```powershell
$env:LIBCLANG_PATH = (Resolve-Path '.tools/llvm-x64/bin').Path
$env:VCPKG_MAX_CONCURRENCY = '4'
cmake --preset windows-msvc-debug -B build/windows-msvc-portable-release `
  '-DCMAKE_CONFIGURATION_TYPES=Release' `
  '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL' `
  '-DCMAKE_MAP_IMPORTED_CONFIG_RELEASE=Release;RelWithDebInfo' `
  '-DSNOW_SHOT_VERSION=0.1.0-preview.1' `
  '-DSNOW_APPS_BUILD_TESTS=OFF' '-DSNOW_APPS_BUILD_BENCHMARKS=OFF' `
  '-DSNOW_APPS_BUILD_IMAGE_VIEWER=OFF' '-DSNOW_APPS_BUILD_SNOW_SHOT_MINI=OFF' `
  '-DSNOW_APPS_API_BASE_URL=https://snowshot.top' `
  '-DVCPKG_MANIFEST_FEATURES=snow-shot' `
  '-DSNOW_IMAGE_PROFILE=redistributable' '-DVCPKG_MANIFEST_INSTALL=ON'
cmake --build build/windows-msvc-portable-release --config Release --target snow_shot --parallel 4
```

The debug preset supplies the shared-kit paths; the explicit configuration and
CRT overrides above produce Release binaries. Do not package the Debug output.
Collect licenses with `scripts/collect-third-party-licenses.ps1` using the build's
resolved vcpkg/Qt prefixes and the FFI, OCR, MCP and updater Cargo manifests.
Then deploy into a new directory using `cmake --install ... --config Release`.
Include the app-local redistributable x64 CRT DLLs and Qt deployment plugins,
including `platforms/qoffscreen.dll` for the isolated update probe.
The install rules omit `tinyxml2.dll` from this shared configuration; copy the
Release DLL from the build output into `bin` and audit recursive PE imports.

Bundle the built OCR worker and its protocol-7 manifest under
`bin/assets/ocr/runtimes/1.2.1/windows-x64`, and the hash-verified default Small
model under `bin/assets/ocr/models/ppocrv6-small-463ea9f`. Regenerate the packaged
OCR asset manifest with this worker's sizes/hashes and corresponding runtime
archive URL; do not retain the upstream binary's hashes for a rebuilt worker.
The two-file OCR repair archive remains ZIP because the native asset manager
uses ZIP. It is an auxiliary asset; the complete runnable software is `.7z`.

## Source and replacement rights

Release source archives include the pinned OCR submodule. Dependency source
archives provide Qt, Cargo packages, vcpkg source archives/build recipes and
MicroTeX; the main source includes project patches and build instructions.
All original component licenses apply. Qt DLLs are dynamically linked and can
be replaced by compatible modified builds; no signature restriction is added.
Microsoft redistributable runtime files remain under Microsoft's terms.

Validate the extracted software with a clean PATH, both distribution/update
probes, MCP discovery, and an interactive screenshot begin/cancel cycle before
publishing. Do not include test data, credentials, logs, PDBs or SDKs in the
software archive. Record the exact validation scope in the release notes.
Run `python scripts/test-snow-shot-portable.py <disposable-extracted-directory>`
for the Windows deployment smoke test. It briefly opens and cancels a screenshot
selection and checks registry snapshots; it does not save captured images.

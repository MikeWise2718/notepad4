# Vendored WebView2 SDK

Headers and loader DLLs for the Markdown preview pane
(see [`specs/markdown-preview-pane.md`](../../specs/markdown-preview-pane.md)).

- **Upstream**: NuGet package `Microsoft.Web.WebView2`
- **Version**: 1.0.3351.48
- **License**: BSD-style, redistribution permitted (see `LICENSE.txt`)

Vendored rather than taken from NuGet because this project has no NuGet
integration — no `packages.config`, no `PackageReference` anywhere in the
solution — and adding it would change how everyone builds the project.

## Contents

| Path | Purpose |
|---|---|
| `include/WebView2.h` | COM interface declarations |
| `include/WebView2EnvironmentOptions.h` | Environment options helper |
| `x64/`, `x86/`, `arm64/` | `WebView2Loader.dll`, one per architecture |

`WebView2LoaderStatic.lib` is **not** vendored: it is ~10 MB per architecture,
and MinGW/GCC cannot link it at all. Notepad4 loads `WebView2Loader.dll`
dynamically instead, so a missing DLL disables the preview rather than
preventing the process from starting.

## The loader DLL must ship with the application

The installed WebView2 *Runtime* does **not** provide `WebView2Loader.dll` —
verified against runtime 151.0.4129.78, which contains `msedgewebview2.exe` but
no loader. The loader comes from the SDK and is the application's
responsibility to distribute. It must sit beside `Notepad4.exe`.

If the DLL is absent the preview pane is simply unavailable; nothing else in the
editor is affected.

## Updating

Download the `Microsoft.Web.WebView2` package, rename the `.nupkg` to `.zip`,
extract, and copy from `build/native/`. Update the version above.

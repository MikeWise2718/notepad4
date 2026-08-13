# WebView2 Markdown Preview — Research Findings

Research conducted 2026-08-13, before any implementation, per the
research-before-build rule for sparsely-documented platform APIs.

Goal: an embedded WebView2 pane in Notepad4 showing a live-rendered preview of the
current Markdown buffer.

## Summary of constraints

Four findings materially shape the design. The first two are hard blockers on
parts of the build matrix; the last two are choices.

| # | Constraint | Impact |
|---|------------|--------|
| 1 | WebView2 requires **Windows 10+** | Notepad4 targets Vista+. Feature must be runtime-gated. |
| 2 | WebView2 **cannot be statically linked under MinGW/GCC** | MinGW builds need dynamic loading, or the feature compiled out. |
| 3 | WebView2 Runtime may be **absent at runtime** | Must fail gracefully, not crash or show an error on startup. |
| 4 | Markdown→HTML needs an embedded library | md4c is the strong candidate (1 source + 1 header, MIT). |

## 1. Platform support — Windows 10 minimum

WebView2 is supported **only on Windows 10 and 11**. It does not support Vista,
7, or 8/8.1 in current versions.

- Windows 7/8 support ended; version 109 was the final runtime for those OSes.
  Runtimes >109 fail to start there.
- Minimum Edge/WebView2 Runtime version to load WebView2 at all: **86.0.616.0**.

**Consequence for Notepad4.** The project targets Vista through 11 (`_WIN32_WINNT_VISTA`
guards throughout `src/Helpers.h`). The preview pane therefore cannot be an
unconditional feature. The codebase already has the right mechanism:
`IsWin10AndAbove()` (`src/Helpers.h:691`) does runtime OS-version gating. The menu
item should be hidden or disabled below Windows 10.

## 2. MinGW/GCC cannot statically link WebView2

> Linking WebView2 statically is possible with Visual C++ but **not** MinGW-w64.
> MinGW-w64/GCC can only link WebView2Loader dynamically.

`WebView2LoaderStatic.lib` is MSVC-only. Notepad4 builds under MSVC, Clang, **and**
MinGW/GCC (`build/mingw/Makefile`), so this splits the build matrix.

Options:

1. **Compile the feature out under MinGW** via an `NP2_ENABLE_*` toggle. Simplest,
   and `src/config.h` already establishes this convention.
2. **Dynamic loading only** — `LoadLibrary` on `WebView2Loader.dll`, resolve
   `CreateCoreWebView2EnvironmentWithOptions` by name. Works on all toolchains,
   requires shipping the DLL.
3. **OpenWebView2Loader** — a third-party reimplementation of the loader. Adds a
   dependency of uncertain maintenance; not recommended.

Note also: linking the official loader is **not strictly required** — a minimal
implementation can be used when `WebView2Loader.dll` is unavailable at runtime.

**Recommendation:** option 1 + 2 combined. Gate compilation behind
`NP2_ENABLE_MARKDOWN_PREVIEW` (default off for MinGW), and within MSVC builds use
runtime dynamic loading so a missing DLL degrades gracefully rather than breaking
process startup.

## 3. No NuGet in this project

The WebView2 SDK and WIL are normally installed via NuGet
(`Microsoft.Web.WebView2`, `Microsoft.Windows.ImplementationLibrary`). Notepad4's
`.vcxproj` files contain **no NuGet references and no `packages.config`** —
verified by grep.

Adding a NuGet dependency would be a meaningful change to how the project builds.
The alternative is vendoring the needed headers (`WebView2.h` and its IDL-generated
companions) into the tree. The MS tutorial also uses WIL (`wil/com.h`) and WRL
(`wrl.h`) for COM smart pointers and the `Callback<>` helper — WRL ships with
MSVC, but WIL is a separate header-only NuGet package.

Notepad4 already uses COM in `src/Bridge.cpp` and `src/Helpers.cpp`, so raw COM
patterns are not foreign to the codebase; WIL may be avoidable.

## 4. Markdown→HTML: md4c

[md4c](https://github.com/mity/md4c) vs [cmark](https://github.com/commonmark/cmark):

| | md4c | cmark |
|---|---|---|
| Size | ~3.7K LOC | ~26.2K LOC |
| Files to embed | `md4c.c/h`, `md4c-html.c/h`, `entity.c/h` | full library |
| License | MIT | BSD-2 |
| API | one function, `md_html()` | AST-based, larger surface |
| Speed | faster | slower |
| CommonMark | fully compliant to 0.31 | reference impl |

md4c is the better fit: it is designed for single-file embedding, is MIT (matching
Notepad4's permissive licensing posture), and has no dependencies beyond libc.

`md_html()` takes Markdown input and invokes a callback with chunks of HTML output
— the callback appends to a buffer. GFM extensions are opt-in flags:
`MD_FLAG_TABLES`, `MD_FLAG_STRIKETHROUGH`, `MD_FLAG_TASKLISTS`, plus footnotes,
wiki-links, and others.

## 5. Win32 embedding sequence

From the official Win32 getting-started guide. Headers: `WebView2.h`, plus `wrl.h`
(for `Callback<>`) and optionally `wil/com.h`.

```cpp
CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
  Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
    [hWnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
      env->CreateCoreWebView2Controller(hWnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [hWnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            controller->get_CoreWebView2(&webview);
            RECT bounds;
            GetClientRect(hWnd, &bounds);
            controller->put_Bounds(bounds);   // repeat on WM_SIZE
            return S_OK;
          }).Get());
      return S_OK;
    }).Get());
```

Key points:

- **Creation is asynchronous**, two nested callbacks deep. The pane cannot be
  assumed ready immediately after the menu command; content set before the
  controller exists must be queued.
- **`put_Bounds`** must be called on every `WM_SIZE` to keep the pane sized to its
  host rect.
- **`NavigateToString`** loads HTML from a string directly — no temp file needed
  for the rendered output. (Note: it has a documented size limit; very large
  documents may need a virtual host mapping instead.)
- The environment call takes a **user data folder**; passing `nullptr` defaults to
  a folder beside the exe, which may fail if the install dir is read-only.
  Notepad4 is often run portable from `D:\ut`, so an explicit writable path under
  `%LOCALAPPDATA%` is safer.

## 6. Runtime-missing behavior

If the WebView2 Runtime is not installed, `CreateCoreWebView2EnvironmentWithOptions`
fails via its completion handler's `HRESULT` rather than crashing. The design must:

- Check the `HRESULT` in the environment callback and show a single, dismissible
  message (or silently disable the pane) rather than repeating an error.
- Never block editor startup on WebView2 initialization — create the pane lazily,
  only when the user first opens the preview.

## Open questions for the spec

1. **Pane layout** — Notepad4 has no existing split-pane infrastructure for the
   editor window. Where does the preview live: a child pane splitting the client
   area, or a separate top-level window? This is likely the largest piece of work,
   independent of WebView2 itself.
2. **Refresh policy** — re-render on every keystroke (debounced), on idle, or on
   explicit command? Scroll synchronization between editor and preview is a
   further increment.
3. **Vendoring vs NuGet** for the WebView2 SDK headers.
4. **Dark theme** — upstream added dark mode stubs (`18d42dd5`). The preview CSS
   should follow the editor theme.
5. **Distribution** — whether `WebView2Loader.dll` ships alongside `Notepad4.exe`.

## Sources

- [WebView2 ending support for Windows 7 and 8/8.1 — Microsoft Edge Blog](https://blogs.windows.com/msedgedev/2022/12/09/microsoft-edge-and-webview2-ending-support-for-windows-7-and-windows-8-8-1/)
- [Minimum WebView2 Runtime version for Windows 7/8 — MS Q&A](https://learn.microsoft.com/en-us/answers/questions/1216969/what-is-the-minimum-version-of-webview2-runtime-th)
- [Get started with WebView2 in Win32 apps — Microsoft Learn](https://learn.microsoft.com/en-us/microsoft-edge/webview2/get-started/win32)
- [ICoreWebView2Controller — Microsoft Learn](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2controller)
- [Custom WebView2Loader implementation — webview/webview PR #783](https://github.com/webview/webview/pull/783)
- [OpenWebView2Loader](https://github.com/jchv/OpenWebView2Loader)
- [md4c — C Markdown parser](https://github.com/mity/md4c)
- [cmark — CommonMark reference implementation](https://github.com/commonmark/cmark)

# Markdown Preview Pane (WebView2)

A split-pane live preview of the current Markdown buffer, rendered by an embedded
WebView2 control.

Research backing this design: [`docs/webview2-markdown-preview-research.md`](../docs/webview2-markdown-preview-research.md).
Read that first — it establishes the platform constraints that shape everything below.

## Decisions taken

| Decision | Choice | Rationale |
|---|---|---|
| Layout | **Split pane** (not separate window) | User preference. Grounded below — `MsgSize` makes this tractable. |
| Refresh | **User-settable option** | Live-debounced / on-idle / on-demand, chosen in settings. |
| Converter | **md4c** vendored into tree | 1 source + 1 header, MIT, no deps. |
| Toolchain gate | `NP2_ENABLE_MARKDOWN_PREVIEW` in `src/config.h` | MinGW can't statically link WebView2. |
| OS gate | `IsWin10AndAbove()` at runtime | WebView2 is Win10+; Notepad4 targets Vista+. |

## Status

| # | Task | Status |
|---|------|--------|
| 1 | Vendor md4c, wire into MSVC + MinGW builds | Not started |
| 2 | Markdown→HTML conversion layer (`MarkdownPreview.cpp`) | Not started |
| 3 | Splitter: extend `MsgSize`, add drag handling | Not started |
| 4 | WebView2 host: lazy init, dynamic loader, graceful degradation | Not started |
| 5 | Menu item, accelerator, `IDM_VIEW_MARKDOWN_PREVIEW` | Not started |
| 6 | Settings persistence (visible, split ratio, refresh mode) | Not started |
| 7 | Refresh-policy implementation (3 modes) | Not started |
| 8 | Dark theme CSS following editor theme | Not started |
| 9 | Build verification across x64/Win32/ARM64/MinGW | Not started |

## Layout integration — the part I was most worried about

Initial concern was that Notepad4 has no split-pane infrastructure. Having read the
code, this is **less severe than expected**.

All editor sizing flows through one function, `MsgSize()` (`src/Notepad4.cpp:2080-2123`).
It subtracts the rebar and statusbar from the client rect and makes exactly one call:

```cpp
SetWindowPos(hwndEdit, nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);   // :2119
```

There is no competing layout logic elsewhere — every other `SetWindowPos` on
`hwndEdit` is a frame-change no-op (`:2061`). So the splitter is a localized change:

1. When the preview is visible, split `cx` into `cxEdit` / `cxSplitter` / `cxPreview`.
2. Position `hwndEdit` in the left portion, the WebView2 host window in the right.
3. Store the split as a **ratio** (not pixels) so it survives resize and DPI change.

A splitter needs a drag handle. Options, in increasing order of work:

- **Bare `SetCapture` handling on the parent** — track `WM_LBUTTONDOWN` /
  `WM_MOUSEMOVE` / `WM_LBUTTONUP` in the gap between panes, set an
  `IDC_SIZEWE` cursor. No new window class. Recommended.
- A dedicated child window for the splitter. Cleaner separation, more code.

DPI: Notepad4 handles `WM_DPICHANGED` (`:2028`) and already scales `cyReBar`; the
splitter width must scale the same way rather than being a fixed pixel constant.

## Component design

### New files

```
src/MarkdownPreview.cpp   — WebView2 host, conversion, refresh scheduling
src/MarkdownPreview.h     — public interface consumed by Notepad4.cpp
src/md4c/                 — vendored: md4c.c/h, md4c-html.c/h, entity.c/h
```

### Public interface (sketch)

```cpp
bool  MarkdownPreview_IsAvailable() noexcept;   // Win10+ && runtime present
void  MarkdownPreview_Toggle(HWND hwndParent);
void  MarkdownPreview_Resize(int x, int y, int cx, int cy);
void  MarkdownPreview_ScheduleRefresh();        // honours refresh mode
void  MarkdownPreview_ApplyTheme(bool darkMode);
```

`Notepad4.cpp` should touch WebView2 types nowhere — all COM stays behind this
header, so the MinGW build compiles the whole thing out cleanly.

### WebView2 initialization

Per the research, creation is **two nested async callbacks deep**. Consequences the
implementation must respect:

- **Lazy init.** Do not create the environment at startup — only on first toggle.
  Startup cost and failure risk stay off the common path.
- **Queue content set before ready.** A refresh arriving before the controller
  exists must be stored and applied in the controller callback, not dropped.
- **`put_Bounds` on every resize**, driven from `MarkdownPreview_Resize`.
- **Explicit user data folder.** Passing `nullptr` defaults to a folder beside the
  exe. Notepad4 runs portable from `D:\ut`, which may be read-only — pass an
  explicit path under `%LOCALAPPDATA%\Notepad4\WebView2`.
- **Dynamic loading.** `LoadLibrary("WebView2Loader.dll")` +
  `GetProcAddress("CreateCoreWebView2EnvironmentWithOptions")` rather than an
  import-lib dependency, so a missing DLL disables the feature instead of
  breaking process startup.

### Failure handling

Three distinct failure points, each needing a *quiet* response — this is a
convenience feature and must never obstruct editing:

| Failure | Response |
|---|---|
| OS < Windows 10 | Menu item hidden entirely. |
| `WebView2Loader.dll` missing / runtime absent | Menu item disabled; one-time message on explicit invoke. |
| Environment callback returns failure `HRESULT` | Show message **once**, disable for the session. |

### Content generation

`md_html()` invokes a callback with HTML chunks; accumulate into a buffer.
Enable GFM flags: `MD_FLAG_TABLES`, `MD_FLAG_STRIKETHROUGH`, `MD_FLAG_TASKLISTS`.

Wrap the fragment in a document with embedded CSS, then `NavigateToString`.

Two caveats:

- **Encoding.** Scintilla holds UTF-8; md4c consumes UTF-8; `NavigateToString`
  takes `LPCWSTR`. One UTF-8→UTF-16 conversion at the boundary.
- **`NavigateToString` has a documented size limit.** Large documents may need a
  virtual host name mapping instead. Treat the string path as the v1 approach and
  measure; note the fallback rather than pre-building it.

## Refresh policy (user-settable)

Three modes, persisted as an integer:

| Mode | Value | Behavior |
|---|---|---|
| Live | 0 | Re-render on modification, debounced ~300 ms via `SetTimer`. |
| On idle | 1 | Re-render after a longer quiet period (~1 s). |
| Manual | 2 | Re-render only on explicit command / save. |

Default: **Live**. Hook the existing `SCN_MODIFIED` notification; the debounce
timer is what keeps large documents from re-rendering on every keystroke.

Scroll synchronization between editor and preview is **explicitly out of scope for
v1** — it is a separate increment and a common source of jitter.

## Settings

Follow the existing `[Settings]` pattern (`src/Notepad4.cpp:5289`, `:5527`):

```cpp
bShowMarkdownPreview   = section.GetBool(L"ShowMarkdownPreview", false);
iMarkdownPreviewSplit  = section.GetInt(L"MarkdownPreviewSplit", 50);    // percent
iMarkdownPreviewRefresh= section.GetInt(L"MarkdownPreviewRefresh", 0);   // mode
```

with matching `SetBoolEx` / `SetIntEx` in the save path.

## Menu and activation

Mirror the `IDM_VIEW_TOOLBAR` / `IDM_VIEW_STATUSBAR` pattern
(`src/resource.h:676`, `src/Notepad4.rc:600`):

- New `IDM_VIEW_MARKDOWN_PREVIEW` in `src/resource.h`
- Menu item in the View menu and the context menu in `src/Notepad4.rc`
- Accelerator — verify a free key rather than assuming; F11 variants are taken

**Visibility rule:** the item should only be meaningful for Markdown documents.
`NP2LEX_MARKDOWN` identifies the lexer (`src/Styles.cpp:1450`). Decide between
hiding it for non-Markdown files vs. leaving it enabled — hiding is cleaner but
surprising if the user is editing an unsaved `.md`-to-be.

## Theming

Upstream added dark mode stubs (`18d42dd5`). Preview CSS should follow the editor
theme rather than being fixed light — read the current background/foreground and
emit matching CSS custom properties. Implement after the pane works; do not block v1 on it.

## Build wiring

- `src/config.h`: `#define NP2_ENABLE_MARKDOWN_PREVIEW 1`, defaulted **0 for MinGW**.
- `build/VisualStudio/Notepad4.vcxproj`: add new sources.
- `build/mingw/Makefile`: add md4c sources (it compiles fine there — only WebView2 doesn't).
- WebView2 SDK headers: **vendor them**, since this project uses no NuGet
  (verified — no `packages.config`, no `PackageReference`).
- Avoid WIL (a separate NuGet header package). WRL ships with MSVC and supplies
  `Callback<>`; the codebase already does raw COM in `src/Bridge.cpp`.

## Open questions

1. **Accelerator key** — needs a free combination; F11 variants are taken.
2. **Non-Markdown files** — hide the menu item, or leave it enabled?
3. **Ship `WebView2Loader.dll`** beside `Notepad4.exe`, or require the system runtime?
4. **`NavigateToString` size ceiling** — measure against a large document before
   committing to the string path.

## Risks

- **WebView2 process overhead.** Each pane spawns browser processes; a lightweight
  editor gaining an Edge instance is a real cost. Lazy init confines it to users
  who ask for it.
- **Upstream divergence.** This is a fork-local feature touching `Notepad4.cpp`,
  `MsgSize`, the menu resources, and settings — all files upstream actively edits.
  Expect rebase conflicts, and keep the footprint in shared files minimal
  (hence all logic behind `MarkdownPreview.h`).
- **Untested build matrix.** Only x64 Release is currently verified against the
  post-rebase tree; Win32/ARM64/MinGW need checking as part of task 9.

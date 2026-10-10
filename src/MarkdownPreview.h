// This file is part of Notepad4.
// See License.txt for details about distribution and modification.
//
// Markdown preview pane. All WebView2 and COM usage is confined to
// MarkdownPreview.cpp so that toolchains which cannot link WebView2
// (MinGW/GCC) compile the feature out cleanly.
//
// See specs\markdown-preview-pane.md.
#pragma once

#include "config.h"

// Refresh policy, persisted as MarkdownPreviewRefresh in [Settings].
enum MarkdownPreviewRefresh {
	MarkdownPreviewRefresh_Live = 0,	// re-render on modification, debounced
	MarkdownPreviewRefresh_Idle = 1,	// re-render after a longer quiet period
	MarkdownPreviewRefresh_Manual = 2,	// re-render only on explicit command
	MarkdownPreviewRefresh_Default = MarkdownPreviewRefresh_Live,
	MarkdownPreviewRefresh_MaxValue = MarkdownPreviewRefresh_Manual,
};

#if NP2_ENABLE_MARKDOWN_PREVIEW

// Convert UTF-8 Markdown to a complete UTF-8 HTML document.
// Returns a NP2HeapAlloc()'d buffer the caller must NP2HeapFree(), or nullptr
// on failure. Available regardless of WebView2 support so it can be tested
// independently of the pane.
//
// mermaid enables diagram rendering for ```mermaid fenced blocks. It only
// affects the generated page; whether the script can actually run also
// depends on MarkdownPreview_IsMermaidAvailable().
//
// lineMarkers inserts an empty <span data-l="N"> before the first rendered
// text of each source line N (0-based), which is what scroll sync and the
// selection highlight navigate by. Off, the output is unchanged.
char *MarkdownPreview_ToHtml(const char *markdown, size_t length, bool darkMode, bool mermaid, bool lineMarkers) noexcept;

// True when mermaid.min.js was found next to the executable. The setting can
// be on while this is false (a stripped-down deployment); diagrams then fall
// back to showing their source as a plain code block.
bool MarkdownPreview_IsMermaidAvailable() noexcept;

// True when the pane can actually be shown: Windows 10 or later, the WebView2
// loader is present, and initialization has not previously failed.
bool MarkdownPreview_IsAvailable() noexcept;

// Show or hide the pane. Creates the WebView2 environment lazily on first show.
void MarkdownPreview_Toggle(HWND hwndParent) noexcept;
bool MarkdownPreview_IsVisible() noexcept;

// True when the pane is showing and its WebView2 child window holds keyboard
// focus. The main message loop uses this to skip the accelerator table so the
// page receives its own editing keys (Ctrl+C, Ctrl+A) instead of having them
// act on the editor behind it.
bool MarkdownPreview_HasFocus() noexcept;

// Position the pane. Called from MsgSize().
void MarkdownPreview_Resize(int x, int y, int cx, int cy) noexcept;

// Queue a re-render according to the current refresh policy. Safe to call
// before the WebView2 controller exists; the update is applied once ready.
void MarkdownPreview_ScheduleRefresh() noexcept;

// Re-render immediately, ignoring the refresh policy debounce.
void MarkdownPreview_Refresh() noexcept;

// Re-apply colors after a theme change.
void MarkdownPreview_ApplyTheme() noexcept;

// Forward the editor's SCN_UPDATEUI flags. With sync on, a scroll moves the
// preview to the same source line and a selection change is echoed as a
// highlight. Cheap to call on every notification; it ignores the rest.
void MarkdownPreview_OnEditorUpdate(unsigned updated) noexcept;

// A different document is now in the editor. The next render opens at the top
// (or at the editor's position, with sync on) instead of where the previous
// document's preview was scrolled.
void MarkdownPreview_ForgetScroll() noexcept;

// Release the WebView2 environment and controller.
void MarkdownPreview_Destroy() noexcept;

#else

#define MarkdownPreview_IsAvailable()			false
#define MarkdownPreview_IsMermaidAvailable()	false
#define MarkdownPreview_IsVisible()				false
#define MarkdownPreview_HasFocus()				false
#define MarkdownPreview_Toggle(hwndParent)		((void)0)
#define MarkdownPreview_Resize(x, y, cx, cy)	((void)0)
#define MarkdownPreview_ScheduleRefresh()		((void)0)
#define MarkdownPreview_Refresh()				((void)0)
#define MarkdownPreview_ApplyTheme()			((void)0)
#define MarkdownPreview_OnEditorUpdate(updated)	((void)0)
#define MarkdownPreview_ForgetScroll()			((void)0)
#define MarkdownPreview_Destroy()				((void)0)

#endif // NP2_ENABLE_MARKDOWN_PREVIEW

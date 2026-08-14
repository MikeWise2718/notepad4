// This file is part of Notepad4.
// See License.txt for details about distribution and modification.
//
// Markdown preview pane backed by WebView2.
//
// All WebView2 and COM usage is confined to this file. Notepad4.cpp sees only
// the plain functions declared in MarkdownPreview.h, so builds which cannot
// link WebView2 (MinGW/GCC) compile the whole feature out.
//
// See specs\markdown-preview-pane.md and
// docs\webview2-markdown-preview-research.md.

#include <windows.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include "config.h"

#if NP2_ENABLE_MARKDOWN_PREVIEW

#include <cstdio>
#include <cstring>
#include <new>
#include "SciCall.h"
#include "Helpers.h"
#include "resource.h"
#include "Notepad4.h"
#include "Dialogs.h"
#include "MarkdownPreview.h"

#include "md4c/md4c.h"
#include "webview2/include/WebView2.h"

// md4c is compiled as C with the default __cdecl convention (see the
// CallingConvention override for these sources in Notepad4.vcxproj), while
// Notepad4 itself is built with __vectorcall. The md4c headers carry no
// explicit convention, so declaring md_html() here with __cdecl keeps the
// declaration matching the definition instead of mangling to __vectorcall.
extern "C" int __cdecl md_html(const MD_CHAR *input, MD_SIZE input_size,
	void (__cdecl *process_output)(const MD_CHAR *, MD_SIZE, void *),
	void *userdata, unsigned parser_flags, unsigned renderer_flags);

extern HWND hwndEdit;
extern int iMarkdownPreviewRefresh;

namespace {

// Debounce intervals, milliseconds.
constexpr UINT RefreshDelayLive = 300;
constexpr UINT RefreshDelayIdle = 1000;

// Grows geometrically; md_html() delivers HTML in many small chunks.
struct HtmlBuffer {
	char *data = nullptr;
	size_t length = 0;
	size_t capacity = 0;
	bool failed = false;
};

void HtmlBuffer_Append(HtmlBuffer *buffer, const char *text, size_t count) noexcept {
	if (buffer->failed) {
		return;
	}
	if (buffer->length + count + 1 > buffer->capacity) {
		size_t capacity = (buffer->capacity == 0) ? 8192 : buffer->capacity;
		while (capacity < buffer->length + count + 1) {
			capacity *= 2;
		}
		char *data = static_cast<char *>(buffer->data == nullptr
			? NP2HeapAlloc(capacity)
			: NP2HeapReAlloc(buffer->data, capacity));
		if (data == nullptr) {
			buffer->failed = true;
			return;
		}
		buffer->data = data;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->length, text, count);
	buffer->length += count;
	buffer->data[buffer->length] = '\0';
}

// __cdecl to match md_html()'s callback type; see the declaration above.
void __cdecl MarkdownOutputCallback(const MD_CHAR *text, MD_SIZE size, void *userdata) noexcept {
	HtmlBuffer_Append(static_cast<HtmlBuffer *>(userdata), text, size);
}

void HtmlBuffer_AppendLiteral(HtmlBuffer *buffer, const char *text) noexcept {
	HtmlBuffer_Append(buffer, text, strlen(text));
}

// The preview follows the editor's own colors rather than the OS theme, so it
// stays consistent with whatever scheme the user has configured.
void GetEditorColors(COLORREF *back, COLORREF *fore) noexcept {
	*back = SciCall_StyleGetBack(STYLE_DEFAULT);
	*fore = SciCall_StyleGetFore(STYLE_DEFAULT);
}

bool IsDarkColor(COLORREF color) noexcept {
	// Rec. 601 luma; below mid-grey counts as dark.
	const int luma = (299 * GetRValue(color) + 587 * GetGValue(color) + 114 * GetBValue(color)) / 1000;
	return luma < 128;
}

void AppendStyleSheet(HtmlBuffer *buffer, bool darkMode) noexcept {
	COLORREF back;
	COLORREF fore;
	GetEditorColors(&back, &fore);

	char css[2048];
	// Colors derived from the editor so the preview matches the active scheme.
	const char *codeBack = darkMode ? "#2d2d2d" : "#f5f5f5";
	const char *borderColor = darkMode ? "#555555" : "#dddddd";
	const char *quoteColor = darkMode ? "#aaaaaa" : "#666666";
	const char *linkColor = darkMode ? "#6cb6ff" : "#0366d6";

	sprintf_s(css, sizeof(css),
		"<style>"
		"html{-webkit-text-size-adjust:100%%;}"
		"body{background:#%02x%02x%02x;color:#%02x%02x%02x;"
		"font-family:'Segoe UI',system-ui,sans-serif;font-size:14px;line-height:1.6;"
		"margin:0;padding:16px 24px;word-wrap:break-word;}"
		"h1,h2,h3,h4,h5,h6{font-weight:600;line-height:1.25;margin:24px 0 16px;}"
		"h1{font-size:2em;border-bottom:1px solid %s;padding-bottom:.3em;}"
		"h2{font-size:1.5em;border-bottom:1px solid %s;padding-bottom:.3em;}"
		"h3{font-size:1.25em;}h4{font-size:1em;}"
		"p,blockquote,ul,ol,dl,table,pre{margin:0 0 16px;}"
		"a{color:%s;text-decoration:none;}a:hover{text-decoration:underline;}"
		"code{background:%s;border-radius:3px;padding:.2em .4em;"
		"font-family:Consolas,'Courier New',monospace;font-size:85%%;}"
		"pre{background:%s;border-radius:4px;padding:12px;overflow:auto;}"
		"pre code{background:none;padding:0;font-size:100%%;}"
		"blockquote{border-left:4px solid %s;color:%s;margin-left:0;padding:0 1em;}"
		"table{border-collapse:collapse;display:block;overflow:auto;}"
		"table th,table td{border:1px solid %s;padding:6px 13px;}"
		"table th{font-weight:600;}"
		"table tr:nth-child(2n){background:%s;}"
		"img{max-width:100%%;}"
		"hr{border:0;border-top:1px solid %s;height:0;margin:24px 0;}"
		"ul,ol{padding-left:2em;}"
		"li.task-list-item{list-style-type:none;margin-left:-1.6em;}"
		"li.task-list-item input{margin-right:.5em;}"
		"</style>",
		GetRValue(back), GetGValue(back), GetBValue(back),
		GetRValue(fore), GetGValue(fore), GetBValue(fore),
		borderColor, borderColor, linkColor,
		codeBack, codeBack, borderColor, quoteColor,
		borderColor, codeBack, borderColor);

	HtmlBuffer_AppendLiteral(buffer, css);
}

} // namespace

char *MarkdownPreview_ToHtml(const char *markdown, size_t length, bool darkMode) noexcept {
	HtmlBuffer buffer;

	HtmlBuffer_AppendLiteral(&buffer,
		"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
		"<meta http-equiv=\"Content-Security-Policy\" "
		"content=\"default-src 'none'; img-src data: https: http:; style-src 'unsafe-inline';\">");
	AppendStyleSheet(&buffer, darkMode);
	HtmlBuffer_AppendLiteral(&buffer, "</head><body>");

	if (length != 0 && markdown != nullptr) {
		// MD_DIALECT_GITHUB enables tables, strikethrough, task lists,
		// permissive autolinks, admonitions and footnotes.
		const int result = md_html(markdown, static_cast<MD_SIZE>(length),
			MarkdownOutputCallback, &buffer, MD_DIALECT_GITHUB, 0);
		if (result != 0) {
			buffer.failed = true;
		}
	}

	HtmlBuffer_AppendLiteral(&buffer, "</body></html>");

	if (buffer.failed) {
		if (buffer.data != nullptr) {
			NP2HeapFree(buffer.data);
		}
		return nullptr;
	}
	return buffer.data;
}

// Convenience wrapper: pull the current document out of Scintilla and render it.
// Returns a NP2HeapAlloc()'d UTF-8 HTML document, or nullptr.
namespace {

char *RenderCurrentDocument() noexcept {
	const Sci_Position docLength = SciCall_GetLength();
	if (docLength < 0) {
		return nullptr;
	}
	const size_t length = static_cast<size_t>(docLength);
	COLORREF back;
	COLORREF fore;
	GetEditorColors(&back, &fore);
	const bool darkMode = IsDarkColor(back);

	if (length == 0) {
		return MarkdownPreview_ToHtml(nullptr, 0, darkMode);
	}

	char *pchText = static_cast<char *>(NP2HeapAlloc(length + 1));
	if (pchText == nullptr) {
		return nullptr;
	}
	SciCall_GetText(length, pchText);

	char *html = MarkdownPreview_ToHtml(pchText, length, darkMode);
	NP2HeapFree(pchText);
	return html;
}

//
// WebView2 host
//
// Raw COM: the handler interfaces are implemented by hand rather than pulling
// in WRL's Callback<> or WIL, both of which would add a NuGet dependency this
// project does not otherwise have.
//

using PFN_CreateCoreWebView2EnvironmentWithOptions = HRESULT (STDAPICALLTYPE *)(
	PCWSTR browserExecutableFolder, PCWSTR userDataFolder,
	ICoreWebView2EnvironmentOptions *environmentOptions,
	ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *environmentCreatedHandler);

using PFN_GetAvailableCoreWebView2BrowserVersionString = HRESULT (STDAPICALLTYPE *)(
	PCWSTR browserExecutableFolder, LPWSTR *versionInfo);

enum PreviewState {
	PreviewState_None,			// nothing attempted yet
	PreviewState_Creating,		// environment/controller creation in flight
	PreviewState_Ready,			// controller live
	PreviewState_Failed,		// creation failed; do not retry this session
};

struct PreviewContext {
	HWND hwndParent = nullptr;
	HMODULE hLoader = nullptr;
	ICoreWebView2Controller *controller = nullptr;
	ICoreWebView2 *webview = nullptr;
	PreviewState state = PreviewState_None;
	bool visible = false;
	bool reported = false;			// error already shown once this session
	RECT bounds{};
	char *pendingHtml = nullptr;	// content produced before the view was ready
};

PreviewContext g_preview;

void ApplyHtml(char *html) noexcept;

// ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
class ControllerHandler final : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
	ULONG STDMETHODCALLTYPE AddRef() noexcept override {
		return InterlockedIncrement(&refCount);
	}
	ULONG STDMETHODCALLTYPE Release() noexcept override {
		const LONG count = InterlockedDecrement(&refCount);
		if (count == 0) {
			delete this;
		}
		return count;
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) noexcept override {
		if (ppv == nullptr) {
			return E_POINTER;
		}
		if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) {
			*ppv = this;
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller *controller) noexcept override {
		if (FAILED(result) || controller == nullptr) {
			g_preview.state = PreviewState_Failed;
			return S_OK;
		}

		g_preview.controller = controller;
		controller->AddRef();
		controller->get_CoreWebView2(&g_preview.webview);

		if (g_preview.webview != nullptr) {
			ICoreWebView2Settings *settings = nullptr;
			if (SUCCEEDED(g_preview.webview->get_Settings(&settings)) && settings != nullptr) {
				// The preview renders local content only: no devtools, no
				// context menu, no status bar, and script is not needed.
				settings->put_IsScriptEnabled(FALSE);
				settings->put_AreDefaultContextMenusEnabled(FALSE);
				settings->put_AreDevToolsEnabled(FALSE);
				settings->put_IsStatusBarEnabled(FALSE);
				settings->put_IsZoomControlEnabled(TRUE);
				settings->Release();
			}
		}

		g_preview.state = PreviewState_Ready;
		controller->put_IsVisible(g_preview.visible);

		// Creation is asynchronous, so the layout pass that ran when the pane
		// was toggled could not size a controller that did not exist yet.
		// Post rather than send: this runs inside a COM callback, and
		// re-entering the window procedure synchronously from here would call
		// back into the controller while it is still being set up.
		if (IsRectEmpty(&g_preview.bounds)) {
			RECT rcClient;
			GetClientRect(g_preview.hwndParent, &rcClient);
			PostMessage(g_preview.hwndParent, WM_SIZE, SIZE_RESTORED,
				MAKELPARAM(rcClient.right, rcClient.bottom));
		} else {
			controller->put_Bounds(g_preview.bounds);
		}

		// Apply whatever was rendered while creation was in flight.
		if (g_preview.pendingHtml != nullptr) {
			char *html = g_preview.pendingHtml;
			g_preview.pendingHtml = nullptr;
			ApplyHtml(html);
		} else {
			MarkdownPreview_Refresh();
		}
		return S_OK;
	}

private:
	LONG refCount = 1;
};

// ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
class EnvironmentHandler final : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
	ULONG STDMETHODCALLTYPE AddRef() noexcept override {
		return InterlockedIncrement(&refCount);
	}
	ULONG STDMETHODCALLTYPE Release() noexcept override {
		const LONG count = InterlockedDecrement(&refCount);
		if (count == 0) {
			delete this;
		}
		return count;
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) noexcept override {
		if (ppv == nullptr) {
			return E_POINTER;
		}
		if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) {
			*ppv = this;
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment *env) noexcept override {
		if (FAILED(result) || env == nullptr) {
			g_preview.state = PreviewState_Failed;
			return S_OK;
		}
		ControllerHandler *handler = new (std::nothrow) ControllerHandler();
		if (handler == nullptr) {
			g_preview.state = PreviewState_Failed;
			return S_OK;
		}
		const HRESULT hr = env->CreateCoreWebView2Controller(g_preview.hwndParent, handler);
		handler->Release();
		if (FAILED(hr)) {
			g_preview.state = PreviewState_Failed;
		}
		return S_OK;
	}

private:
	LONG refCount = 1;
};

// The user data folder must be writable. Notepad4 is often run portable from a
// directory the user cannot write to, so keep browser state under LOCALAPPDATA
// rather than letting WebView2 default to a folder beside the executable.
bool GetUserDataFolder(LPWSTR path, DWORD cch) noexcept {
	WCHAR local[MAX_PATH];
	if (FAILED(SHGetFolderPath(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local))) {
		return false;
	}
	if (_snwprintf_s(path, cch, _TRUNCATE, L"%s\\Notepad4\\WebView2", local) < 0) {
		return false;
	}
	SHCreateDirectoryEx(nullptr, path, nullptr);
	return true;
}

HMODULE LoadWebView2Loader() noexcept {
	if (g_preview.hLoader != nullptr) {
		return g_preview.hLoader;
	}
	// Beside the executable only: never search the working directory or PATH,
	// which would be a DLL planting vector.
	WCHAR path[MAX_PATH];
	const DWORD length = GetModuleFileName(nullptr, path, COUNTOF(path));
	if (length == 0 || length >= COUNTOF(path)) {
		return nullptr;
	}
	LPWSTR name = PathFindFileName(path);
	if (name == nullptr) {
		return nullptr;
	}
	lstrcpy(name, L"WebView2Loader.dll");
	g_preview.hLoader = LoadLibraryEx(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	return g_preview.hLoader;
}

void ApplyHtml(char *html) noexcept {
	if (html == nullptr) {
		return;
	}
	if (g_preview.state != PreviewState_Ready || g_preview.webview == nullptr) {
		// Creation still in flight: keep only the most recent render.
		if (g_preview.pendingHtml != nullptr) {
			NP2HeapFree(g_preview.pendingHtml);
		}
		g_preview.pendingHtml = html;
		return;
	}

	// NavigateToString() is documented to reject strings above roughly 2 MB.
	// Truncate rather than letting the call fail on a large document.
	constexpr size_t MaxNavigateBytes = 1536 * 1024;
	size_t htmlLength = strlen(html);
	if (htmlLength > MaxNavigateBytes) {
		htmlLength = MaxNavigateBytes;
		// Do not cut a UTF-8 sequence in half; back up to a lead byte.
		while (htmlLength != 0 && (static_cast<unsigned char>(html[htmlLength]) & 0xC0) == 0x80) {
			--htmlLength;
		}
		html[htmlLength] = '\0';
	}

	const int cchWide = MultiByteToWideChar(CP_UTF8, 0, html, -1, nullptr, 0);
	if (cchWide > 0) {
		LPWSTR wide = static_cast<LPWSTR>(NP2HeapAlloc(static_cast<size_t>(cchWide) * sizeof(WCHAR)));
		if (wide != nullptr) {
			MultiByteToWideChar(CP_UTF8, 0, html, -1, wide, cchWide);
			g_preview.webview->NavigateToString(wide);
			NP2HeapFree(wide);
		}
	}
	NP2HeapFree(html);
}

} // namespace

bool MarkdownPreview_IsAvailable() noexcept {
	if (g_preview.state == PreviewState_Failed) {
		return false;
	}
	if (!IsWin10AndAbove()) {
		return false;
	}
	const HMODULE hLoader = LoadWebView2Loader();
	if (hLoader == nullptr) {
		return false;
	}
	// The loader being present does not mean the Runtime is installed.
	const auto pfnVersion = reinterpret_cast<PFN_GetAvailableCoreWebView2BrowserVersionString>(
		reinterpret_cast<void *>(GetProcAddress(hLoader, "GetAvailableCoreWebView2BrowserVersionString")));
	if (pfnVersion == nullptr) {
		return false;
	}
	LPWSTR version = nullptr;
	if (FAILED(pfnVersion(nullptr, &version)) || version == nullptr) {
		return false;
	}
	CoTaskMemFree(version);
	return true;
}

bool MarkdownPreview_IsVisible() noexcept {
	return g_preview.visible;
}

void MarkdownPreview_Toggle(HWND hwndParent) noexcept {
	if (g_preview.visible) {
		g_preview.visible = false;
		if (g_preview.controller != nullptr) {
			g_preview.controller->put_IsVisible(FALSE);
		}
		return;
	}

	if (!MarkdownPreview_IsAvailable()) {
		if (!g_preview.reported) {
			g_preview.reported = true;
			MsgBoxWarn(MB_OK, IDS_MARKDOWN_PREVIEW_UNAVAILABLE);
		}
		return;
	}

	g_preview.visible = true;
	g_preview.hwndParent = hwndParent;

	if (g_preview.state == PreviewState_Ready && g_preview.controller != nullptr) {
		g_preview.controller->put_IsVisible(TRUE);
		MarkdownPreview_Refresh();
		return;
	}
	if (g_preview.state == PreviewState_Creating) {
		return;	// creation already in flight
	}

	// Lazy creation: only the first time the pane is actually shown.
	const HMODULE hLoader = LoadWebView2Loader();
	const auto pfnCreate = reinterpret_cast<PFN_CreateCoreWebView2EnvironmentWithOptions>(
		reinterpret_cast<void *>(GetProcAddress(hLoader, "CreateCoreWebView2EnvironmentWithOptions")));
	if (pfnCreate == nullptr) {
		g_preview.state = PreviewState_Failed;
		g_preview.visible = false;
		return;
	}

	WCHAR userDataFolder[MAX_PATH];
	if (!GetUserDataFolder(userDataFolder, COUNTOF(userDataFolder))) {
		g_preview.state = PreviewState_Failed;
		g_preview.visible = false;
		return;
	}

	EnvironmentHandler *handler = new (std::nothrow) EnvironmentHandler();
	if (handler == nullptr) {
		g_preview.state = PreviewState_Failed;
		g_preview.visible = false;
		return;
	}

	g_preview.state = PreviewState_Creating;
	const HRESULT hr = pfnCreate(nullptr, userDataFolder, nullptr, handler);
	handler->Release();
	if (FAILED(hr)) {
		g_preview.state = PreviewState_Failed;
		g_preview.visible = false;
	}
}

void MarkdownPreview_Resize(int x, int y, int cx, int cy) noexcept {
	SetRect(&g_preview.bounds, x, y, x + cx, y + cy);
	if (g_preview.controller != nullptr) {
		g_preview.controller->put_Bounds(g_preview.bounds);
	}
}

void MarkdownPreview_Refresh() noexcept {
	if (!g_preview.visible) {
		return;
	}
	ApplyHtml(RenderCurrentDocument());
}

void MarkdownPreview_ScheduleRefresh() noexcept {
	if (!g_preview.visible || g_preview.hwndParent == nullptr) {
		return;
	}
	switch (iMarkdownPreviewRefresh) {
	case MarkdownPreviewRefresh_Live:
		// Restarting the timer on each change coalesces bursts of typing into
		// a single render once the user pauses.
		SetTimer(g_preview.hwndParent, ID_MARKDOWNPREVIEWTIMER, RefreshDelayLive, nullptr);
		break;
	case MarkdownPreviewRefresh_Idle:
		SetTimer(g_preview.hwndParent, ID_MARKDOWNPREVIEWTIMER, RefreshDelayIdle, nullptr);
		break;
	default:
		break;	// manual: only explicit commands refresh
	}
}

void MarkdownPreview_ApplyTheme() noexcept {
	MarkdownPreview_Refresh();
}

void MarkdownPreview_Destroy() noexcept {
	if (g_preview.pendingHtml != nullptr) {
		NP2HeapFree(g_preview.pendingHtml);
		g_preview.pendingHtml = nullptr;
	}
	if (g_preview.webview != nullptr) {
		g_preview.webview->Release();
		g_preview.webview = nullptr;
	}
	if (g_preview.controller != nullptr) {
		g_preview.controller->Close();
		g_preview.controller->Release();
		g_preview.controller = nullptr;
	}
	// The loader DLL is intentionally left loaded: unloading it while the
	// browser process is shutting down has been a source of crashes.
	g_preview.state = PreviewState_None;
	g_preview.visible = false;
}

#endif // NP2_ENABLE_MARKDOWN_PREVIEW

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
#include "SciCall.h"
#include "Helpers.h"
#include "resource.h"
#include "MarkdownPreview.h"

#include "md4c/md4c.h"

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
char *MarkdownPreview_RenderDocument() noexcept {
	const size_t length = SciCall_GetLength();
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

#endif // NP2_ENABLE_MARKDOWN_PREVIEW

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
#include <cstdint>
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
extern bool bMarkdownPreviewMermaid;
extern bool bMarkdownPreviewSync;

// Virtual host the preview page loads its bundled assets from. The .invalid
// TLD is reserved by RFC 2606 and can never resolve on the real network, so a
// mapping failure degrades to a failed load rather than an outbound request.
#define MERMAID_VIRTUAL_HOST	L"notepad4.invalid"
#define MERMAID_SCRIPT_URL		"https://notepad4.invalid/mermaid.min.js"

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

//
// Source-line markers
//
// md4c reports no source positions. Its HTML renderer, though, hands unescaped
// text to the output callback as pointers into the original Markdown buffer
// (render_html_escaped() emits each run with render_verbatim(r, data + beg,
// ...)), while every tag arrives as a string literal. So a chunk that lies
// inside the input says exactly which source line produced it, and markers can
// be added here without touching the vendored md4c sources.
//
// A marker may only go where an element is allowed. Input text also lands
// inside attributes (image alt text, link URLs, a code block's language), and
// raw HTML can be split across lines in the middle of a tag; in raw-text
// elements such as <style> a marker would appear as literal text. A small state
// machine over everything emitted tracks where we are. It is reliable for what
// md4c generates itself, which always escapes < > and " in text.
//

enum MarkupState : unsigned char {
	Markup_Content,
	Markup_TagName,		// just after '<', collecting the element name
	Markup_Tag,			// inside a tag, outside quotes
	Markup_TagDQ,		// inside a double-quoted attribute value
	Markup_TagSQ,		// inside a single-quoted attribute value
	Markup_RawText,		// inside <style>, <script>, <textarea> and the like
};

struct RenderContext {
	HtmlBuffer *buffer = nullptr;
	// Marker support; lineStarts is null when markers are off.
	const char *input = nullptr;
	size_t inputLength = 0;
	size_t *lineStarts = nullptr;	// byte offset of the first byte of each line
	size_t lineCount = 0;
	size_t lastMarkerLine = SIZE_MAX;
	MarkupState state = Markup_Content;
	bool closingTag = false;
	bool rawTextTag = false;		// the tag being parsed opens a raw-text element
	unsigned nameLength = 0;
	unsigned rawMatched = 0;		// progress through "</name" in raw text
	char name[12]{};
};

// Elements whose content is not parsed as markup, so a marker inside would be
// shown (or executed) as text rather than ignored.
bool IsRawTextElement(const char *name) noexcept {
	static const char * const elements[] = {
		"script", "style", "textarea", "title", "xmp", "iframe", "noembed", "noframes", "noscript",
	};
	for (const char *element : elements) {
		if (strcmp(name, element) == 0) {
			return true;
		}
	}
	return false;
}

inline char AsciiLower(char ch) noexcept {
	return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

inline bool IsTagNameChar(char ch) noexcept {
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '!';
}

void FinishTag(RenderContext *ctx) noexcept {
	if (ctx->rawTextTag) {
		ctx->state = Markup_RawText;
		ctx->rawMatched = 0;
	} else {
		ctx->state = Markup_Content;
	}
	ctx->rawTextTag = false;
}

void AdvanceMarkupState(RenderContext *ctx, char ch) noexcept {
	switch (ctx->state) {
	case Markup_Content:
		if (ch == '<') {
			ctx->state = Markup_TagName;
			ctx->closingTag = false;
			ctx->nameLength = 0;
		}
		break;

	case Markup_TagName:
		if (ch == '/' && ctx->nameLength == 0 && !ctx->closingTag) {
			ctx->closingTag = true;
		} else if (IsTagNameChar(ch)) {
			if (ctx->nameLength + 1 < sizeof(ctx->name)) {
				ctx->name[ctx->nameLength++] = AsciiLower(ch);
			}
		} else {
			ctx->name[ctx->nameLength] = '\0';
			ctx->rawTextTag = !ctx->closingTag && IsRawTextElement(ctx->name);
			if (ch == '>') {
				FinishTag(ctx);
			} else if (ch == '"') {
				ctx->state = Markup_TagDQ;
			} else if (ch == '\'') {
				ctx->state = Markup_TagSQ;
			} else {
				ctx->state = Markup_Tag;
			}
		}
		break;

	case Markup_Tag:
		if (ch == '"') {
			ctx->state = Markup_TagDQ;
		} else if (ch == '\'') {
			ctx->state = Markup_TagSQ;
		} else if (ch == '>') {
			FinishTag(ctx);
		}
		break;

	case Markup_TagDQ:
		if (ch == '"') {
			ctx->state = Markup_Tag;
		}
		break;

	case Markup_TagSQ:
		if (ch == '\'') {
			ctx->state = Markup_Tag;
		}
		break;

	case Markup_RawText: {
		// Look for "</name" case-insensitively, then let the tag states
		// consume the rest of the closing tag.
		const unsigned length = 2 + ctx->nameLength;
		const char expected = (ctx->rawMatched == 0) ? '<'
			: ((ctx->rawMatched == 1) ? '/' : ctx->name[ctx->rawMatched - 2]);
		if (AsciiLower(ch) == expected) {
			if (++ctx->rawMatched == length) {
				ctx->state = Markup_Tag;
				ctx->rawTextTag = false;
			}
		} else {
			ctx->rawMatched = (ch == '<') ? 1 : 0;
		}
	} break;
	}
}

bool IsLineBreakAt(const RenderContext *ctx, size_t offset) noexcept {
	const char ch = ctx->input[offset];
	// CRLF counts once, at the LF; a lone CR (classic Mac) also ends a line,
	// matching how Scintilla numbers lines.
	return ch == '\n' || (ch == '\r' && (offset + 1 >= ctx->inputLength || ctx->input[offset + 1] != '\n'));
}

bool BuildLineStarts(RenderContext *ctx) noexcept {
	size_t count = 1;
	for (size_t i = 0; i < ctx->inputLength; i++) {
		count += IsLineBreakAt(ctx, i);
	}
	ctx->lineStarts = static_cast<size_t *>(NP2HeapAlloc(count * sizeof(size_t)));
	if (ctx->lineStarts == nullptr) {
		return false;
	}
	size_t line = 0;
	ctx->lineStarts[line++] = 0;
	for (size_t i = 0; i < ctx->inputLength; i++) {
		if (IsLineBreakAt(ctx, i)) {
			ctx->lineStarts[line++] = i + 1;
		}
	}
	ctx->lineCount = count;
	return true;
}

size_t LineFromOffset(const RenderContext *ctx, size_t offset) noexcept {
	// Chunks mostly arrive in source order, but not always (footnotes are
	// emitted at the end), so a search rather than a running count.
	// Last line starting at or before offset; lineStarts[0] is 0, so one exists.
	size_t lo = 0;
	size_t hi = ctx->lineCount;
	while (hi - lo > 1) {
		const size_t mid = lo + (hi - lo) / 2;
		if (ctx->lineStarts[mid] <= offset) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return lo;
}

void AppendLineMarker(HtmlBuffer *buffer, size_t line) noexcept {
	char marker[48];
	const int length = sprintf_s(marker, sizeof(marker), "<span data-l=\"%zu\"></span>", line);
	if (length > 0) {
		HtmlBuffer_Append(buffer, marker, static_cast<size_t>(length));
	}
}

// __cdecl to match md_html()'s callback type; see the declaration above.
void __cdecl MarkdownOutputCallback(const MD_CHAR *text, MD_SIZE size, void *userdata) noexcept {
	RenderContext *ctx = static_cast<RenderContext *>(userdata);
	if (ctx->lineStarts == nullptr) {
		HtmlBuffer_Append(ctx->buffer, text, size);
		return;
	}

	// Compared as integers: the chunk is either inside the input or an
	// unrelated literal, and relational operators on unrelated pointers are
	// not meaningful.
	const uintptr_t base = reinterpret_cast<uintptr_t>(ctx->input);
	const uintptr_t start = reinterpret_cast<uintptr_t>(text);
	const bool inInput = start >= base && start - base + size <= ctx->inputLength;
	const size_t offset = inInput ? static_cast<size_t>(start - base) : 0;
	size_t line = inInput ? LineFromOffset(ctx, offset) : 0;

	size_t flushed = 0;
	for (size_t i = 0; i < size; i++) {
		const char ch = text[i];
		if (inInput && line != ctx->lastMarkerLine && ctx->state == Markup_Content && ch != '\n' && ch != '\r') {
			HtmlBuffer_Append(ctx->buffer, text + flushed, i - flushed);
			flushed = i;
			AppendLineMarker(ctx->buffer, line);
			ctx->lastMarkerLine = line;
		}
		AdvanceMarkupState(ctx, ch);
		if (inInput && IsLineBreakAt(ctx, offset + i)) {
			++line;
		}
	}
	HtmlBuffer_Append(ctx->buffer, text + flushed, size - flushed);
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

// A per-page nonce is what lets the one inline script run while inline script
// stays forbidden for everything else. It must be unpredictable: a fixed or
// guessable value would let script inside the document being previewed opt
// itself in. RtlGenRandom (exposed as SystemFunction036) is used directly to
// avoid pulling in bcrypt for a 16-byte draw; it is present on every supported
// Windows version. On failure the caller renders without Mermaid rather than
// falling back to a weak nonce.
bool GenerateNonce(char *out, size_t size) noexcept {
	using PFN_RtlGenRandom = BOOLEAN (WINAPI *)(PVOID, ULONG);
	static PFN_RtlGenRandom pfnRtlGenRandom = nullptr;
	if (pfnRtlGenRandom == nullptr) {
		// advapi32 is already linked and loaded; this only takes its address.
		const HMODULE hAdvapi = GetModuleHandle(L"advapi32.dll");
		if (hAdvapi == nullptr) {
			return false;
		}
		pfnRtlGenRandom = reinterpret_cast<PFN_RtlGenRandom>(
			reinterpret_cast<void *>(GetProcAddress(hAdvapi, "SystemFunction036")));
		if (pfnRtlGenRandom == nullptr) {
			return false;
		}
	}

	BYTE bytes[16];
	if (!pfnRtlGenRandom(bytes, sizeof(bytes))) {
		return false;
	}
	// Hex rather than base64: no '+' or '/' to escape inside an attribute.
	if (size < 2 * sizeof(bytes) + 1) {
		return false;
	}
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < sizeof(bytes); i++) {
		out[2 * i] = digits[bytes[i] >> 4];
		out[2 * i + 1] = digits[bytes[i] & 0x0f];
	}
	out[2 * sizeof(bytes)] = '\0';
	return true;
}

// Diagrams are centred on their own background. Until the script has run, a
// mermaid block is still a <pre> full of source; hiding it via visibility
// (not display) keeps its height, so the page does not jump when it renders.
void AppendMermaidStyle(HtmlBuffer *buffer) noexcept {
	HtmlBuffer_AppendLiteral(buffer,
		"<style>"
		"pre.mermaid{background:none;padding:0;text-align:center;visibility:hidden;}"
		"pre.mermaid[data-processed]{visibility:visible;}"
		"pre.mermaid svg{max-width:100%;height:auto;}"
		".mermaid-error{border-left:4px solid #d73a49;padding:.5em 1em;margin:0 0 16px;"
		"font-family:Consolas,'Courier New',monospace;font-size:85%;white-space:pre-wrap;}"
		"</style>");
}

// md4c renders a ```mermaid fence as <pre><code class="language-mermaid">.
// Mermaid expects <pre class="mermaid"> holding the raw diagram source, so the
// blocks are rewritten before mermaid.run() is called.
//
// Diagrams are rendered one at a time rather than by a bare mermaid.run() over
// the whole page: a single malformed diagram otherwise aborts the batch and
// leaves every later diagram unrendered, which is a poor experience while the
// document is being edited live.
void AppendMermaidScript(HtmlBuffer *buffer, bool darkMode, const char *nonce) noexcept {
	HtmlBuffer_AppendLiteral(buffer, "<script src=\"" MERMAID_SCRIPT_URL "\"></script>");
	// The nonce is what makes this one inline block executable; without it the
	// page's own CSP blocks it, exactly as it blocks any script in the document.
	HtmlBuffer_AppendLiteral(buffer, "<script nonce=\"");
	HtmlBuffer_AppendLiteral(buffer, nonce);
	HtmlBuffer_AppendLiteral(buffer, "\">(function(){");
	HtmlBuffer_AppendLiteral(buffer,
		"if(typeof mermaid==='undefined'){return;}"
		"var blocks=document.querySelectorAll('pre>code.language-mermaid');"
		"if(blocks.length===0){return;}"
		"var nodes=[];"
		"for(var i=0;i<blocks.length;i++){"
			"var pre=blocks[i].parentNode;"
			// textContent, not innerHTML: md4c has already escaped the source,
			// and mermaid needs it back in its original form.
			"pre.textContent=blocks[i].textContent;"
			"pre.className='mermaid';"
			"nodes.push(pre);"
		"}");
	// antiscript, not strict: labels in real documents use <b> and <br/> for
	// formatting, which strict discards. antiscript keeps those while still
	// removing script. Not 'loose', which would also allow click handlers and
	// arbitrary HTML from whatever file happens to be open.
	//
	// suppressErrorRendering stops mermaid replacing a failed diagram with its
	// own "syntax error" bomb graphic: the message added below is more precise
	// and far less intrusive while a diagram is being typed.
	HtmlBuffer_AppendLiteral(buffer, darkMode
		? "mermaid.initialize({startOnLoad:false,theme:'dark',securityLevel:'antiscript',suppressErrorRendering:true});"
		: "mermaid.initialize({startOnLoad:false,theme:'default',securityLevel:'antiscript',suppressErrorRendering:true});");
	HtmlBuffer_AppendLiteral(buffer,
		"(async function(){"
			"for(var i=0;i<nodes.length;i++){"
				"try{"
					"await mermaid.run({nodes:[nodes[i]]});"
				"}catch(e){"
					// Show the failure in place, keeping the source visible so
					// the diagram being typed can be corrected.
					"var msg=document.createElement('pre');"
					"msg.className='mermaid-error';"
					"msg.textContent='Mermaid: '+(e&&e.message?e.message:e);"
					"nodes[i].parentNode.insertBefore(msg,nodes[i]);"
					"nodes[i].style.visibility='visible';"
				"}"
			"}"
		"})();");
	HtmlBuffer_AppendLiteral(buffer, "})();</script>");
}

} // namespace

char *MarkdownPreview_ToHtml(const char *markdown, size_t length, bool darkMode, bool mermaid, bool lineMarkers) noexcept {
	HtmlBuffer buffer;

	// A failed nonce draw disables Mermaid rather than weakening the policy.
	char nonce[33];
	if (mermaid && !GenerateNonce(nonce, sizeof(nonce))) {
		mermaid = false;
	}

	HtmlBuffer_AppendLiteral(&buffer,
		"<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
	// Mermaid needs to run the bundled script from the virtual host, its own
	// nonce'd bootstrap, and the <style> elements it injects. Inline script is
	// allowed only for that one nonce, so script inside the document being
	// previewed still cannot execute.
	if (mermaid) {
		HtmlBuffer_AppendLiteral(&buffer,
			"<meta http-equiv=\"Content-Security-Policy\" "
			"content=\"default-src 'none'; img-src data: https: http:; "
			"style-src 'unsafe-inline'; font-src data:; "
			"script-src https://notepad4.invalid 'nonce-");
		HtmlBuffer_AppendLiteral(&buffer, nonce);
		HtmlBuffer_AppendLiteral(&buffer, "';\">");
	} else {
		HtmlBuffer_AppendLiteral(&buffer,
			"<meta http-equiv=\"Content-Security-Policy\" "
			"content=\"default-src 'none'; img-src data: https: http:; style-src 'unsafe-inline';\">");
	}
	AppendStyleSheet(&buffer, darkMode);
	if (mermaid) {
		AppendMermaidStyle(&buffer);
	}
	if (lineMarkers) {
		// The editor selection echoed into the preview, painted with the CSS
		// Custom Highlight API so it never alters the document's layout.
		HtmlBuffer_AppendLiteral(&buffer, darkMode
			? "<style>::highlight(np4-echo){background-color:rgba(255,200,0,.32);}</style>"
			: "<style>::highlight(np4-echo){background-color:rgba(255,210,0,.45);}</style>");
	}
	HtmlBuffer_AppendLiteral(&buffer, "</head><body>");

	if (length != 0 && markdown != nullptr) {
		RenderContext ctx;
		ctx.buffer = &buffer;
		if (lineMarkers) {
			ctx.input = markdown;
			ctx.inputLength = length;
			// Without the table the document still renders, just unmarked.
			BuildLineStarts(&ctx);
		}
		// MD_DIALECT_GITHUB enables tables, strikethrough, task lists,
		// permissive autolinks, admonitions and footnotes.
		const int result = md_html(markdown, static_cast<MD_SIZE>(length),
			MarkdownOutputCallback, &ctx, MD_DIALECT_GITHUB, 0);
		if (ctx.lineStarts != nullptr) {
			NP2HeapFree(ctx.lineStarts);
		}
		if (result != 0) {
			buffer.failed = true;
		}
	}

	if (mermaid) {
		AppendMermaidScript(&buffer, darkMode, nonce);
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

// Defined below, next to the other path helpers.
bool GetAssetFolder(LPWSTR path, DWORD cch) noexcept;

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
	// Requested by the user and actually present on disk.
	const bool mermaid = bMarkdownPreviewMermaid && MarkdownPreview_IsMermaidAvailable();

	// Markers are only worth their bytes when something navigates by them.
	const bool lineMarkers = bMarkdownPreviewSync;

	if (length == 0) {
		return MarkdownPreview_ToHtml(nullptr, 0, darkMode, mermaid, lineMarkers);
	}

	char *pchText = static_cast<char *>(NP2HeapAlloc(length + 1));
	if (pchText == nullptr) {
		return nullptr;
	}
	SciCall_GetText(length, pchText);

	char *html = MarkdownPreview_ToHtml(pchText, length, darkMode, mermaid, lineMarkers);
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
	bool focused = false;			// the WebView2 child window holds focus
	RECT bounds{};
	char *pendingHtml = nullptr;	// content produced before the view was ready

	// Scroll sync and scroll preservation; see the page script below.
	bool pageReady = false;			// our page has loaded and np4 is injected
	bool syncInFlight = false;		// an np4.update() call has not completed yet
	bool syncDirty = false;			// the editor moved again while it was in flight
	bool scrollQueryInFlight = false;
	bool forgetScroll = false;		// the next render is a different document
	int restoreScrollY = -1;		// where to put the next page, sync off
	char *queuedHtml = nullptr;		// waiting on the scroll position query
	// Last state sent to the page, so caret moves within a line cost nothing.
	int sentLine = -1;
	int sentFrac = -1;
	int sentFlags = -1;
	int sentSelFrom = -2;
	int sentSelTo = -2;
};

PreviewContext g_preview;

void ApplyHtml(char *html) noexcept;
void NavigateQueued() noexcept;
void SendEditorState(bool force) noexcept;

// The sync script, injected with ExecuteScript() once each page has loaded.
//
// Injected by the host rather than shipped in the page: WebView2 runs
// ExecuteScript() code even with page script disabled, and the page's CSP does
// not apply to it. So this needs no change to when the page itself may run
// script -- still only for Mermaid -- and nothing in the previewed document can
// call into it or post anything back. Host to page, one way.
//
// np4.update(line, frac, top, bottom, total, selFrom, selTo)
//   Scroll so source line `line`, plus `frac` of it when word-wrapped, is at
//   the top, interpolating between the nearest markers either side so tall
//   blocks (tables, diagrams) scroll smoothly rather than in jumps. `top` and
//   `bottom` pin the two ends exactly. selFrom..selTo are the selected source
//   lines, painted as a highlight; -1 clears it.
// np4.restore(y)
//   Sync off: put a refreshed page back where the reader had it.
//
// Layout keeps moving after load (Mermaid renders asynchronously, images
// arrive late), so a ResizeObserver re-applies the last request -- until the
// reader takes the preview over with the mouse or keyboard.
//
// Markers are filtered to those increasing in both line and position, which
// drops the ones a footnote section places at the end of the page.
constexpr WCHAR SyncScript[] =
	L"(function(){"
	L"if(window.np4){return;}"
	L"var marks=null,target=null,restoreY=-1,held=false,lastA=-2,lastB=-2;"
	L"function maxY(){return Math.max(0,document.documentElement.scrollHeight-window.innerHeight);}"
	L"function collect(){"
		L"var all=document.querySelectorAll('span[data-l]'),out=[],ll=-1,ly=-1,sy=window.scrollY;"
		L"for(var i=0;i<all.length;i++){"
			L"var e=all[i];"
			L"if(!e.getClientRects().length){continue;}"
			L"var l=+e.getAttribute('data-l'),y=e.getBoundingClientRect().top+sy;"
			L"if(l>ll&&y>=ly){out.push(l,y);ll=l;ly=y;}"
		L"}"
		L"return out;"
	L"}"
	L"function lineY(t){"
		L"if(!marks){marks=collect();}"
		L"var n=marks.length>>1;"
		L"if(!n){return -1;}"
		L"var a=0,b=n-1,lo=-1;"
		L"while(a<=b){var m=(a+b)>>1;if(marks[2*m]<=t.line){lo=m;a=m+1;}else{b=m-1;}}"
		L"var l0=0,y0=0,l1,y1;"
		L"if(lo>=0){l0=marks[2*lo];y0=marks[2*lo+1];}"
		L"if(lo+1<n){l1=marks[2*lo+2];y1=marks[2*lo+3];}"
		L"else{l1=Math.max(t.total,l0+1);y1=document.documentElement.scrollHeight;}"
		L"return y0+(y1-y0)*(t.line+t.frac-l0)/Math.max(1,l1-l0);"
	L"}"
	L"function apply(){"
		L"var y;"
		L"if(target){y=target.top?0:(target.bottom?maxY():lineY(target));}"
		L"else if(restoreY>=0&&!held){y=restoreY;}"
		L"else{return;}"
		L"if(y>=0){window.scrollTo(0,Math.min(y,maxY()));}"
	L"}"
	L"function mark(a,b){"
		L"if(a===lastA&&b===lastB){return;}"
		L"lastA=a;lastB=b;"
		L"if(!(window.CSS&&CSS.highlights&&window.Highlight)){return;}"
		L"CSS.highlights.delete('np4-echo');"
		L"if(a<0){return;}"
		L"var all=document.querySelectorAll('span[data-l]'),s=null,e=null;"
		L"for(var i=0;i<all.length;i++){"
			L"var l=+all[i].getAttribute('data-l');"
			L"if(!s){if(l>=a&&l<=b){s=all[i];}}else if(l>b){e=all[i];break;}"
		L"}"
		L"if(!s){return;}"
		L"var r=document.createRange();"
		L"r.setStartBefore(s);"
		L"if(e){r.setEndBefore(e);}else{r.setEnd(document.body,document.body.childNodes.length);}"
		L"CSS.highlights.set('np4-echo',new Highlight(r));"
	L"}"
	L"function hold(){held=true;target=null;}"
	L"['wheel','mousedown','keydown','touchstart'].forEach(function(t){"
		L"window.addEventListener(t,hold,{capture:true,passive:true});"
	L"});"
	L"if(window.ResizeObserver){new ResizeObserver(function(){marks=null;apply();}).observe(document.body);}"
	L"window.np4={"
		L"update:function(line,frac,top,bottom,total,a,b){"
			L"target={line:line,frac:frac,top:top,bottom:bottom,total:total};"
			L"held=false;apply();mark(a,b);"
		L"},"
		L"restore:function(y){restoreY=y;held=false;apply();}"
	L"};"
	L"})();";

// ICoreWebView2ExecuteScriptCompletedHandler, for the two calls whose
// completion matters: an np4.update() (to coalesce editor movement into one
// call in flight at a time) and the scroll query made before a refresh.
enum ScriptKind {
	Script_Sync,
	Script_ScrollQuery,
};

class ScriptHandler final : public ICoreWebView2ExecuteScriptCompletedHandler {
public:
	explicit ScriptHandler(ScriptKind kind) noexcept : kind{kind} {}

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
		if (riid == IID_IUnknown || riid == IID_ICoreWebView2ExecuteScriptCompletedHandler) {
			*ppv = this;
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, LPCWSTR resultObjectAsJson) noexcept override {
		if (kind == Script_Sync) {
			g_preview.syncInFlight = false;
			if (g_preview.syncDirty) {
				g_preview.syncDirty = false;
				SendEditorState(false);
			}
			return S_OK;
		}

		g_preview.scrollQueryInFlight = false;
		if (SUCCEEDED(errorCode) && resultObjectAsJson != nullptr) {
			// A JSON number, possibly fractional; "null" if the page had none.
			LPWSTR end = nullptr;
			const double y = wcstod(resultObjectAsJson, &end);
			if (end != resultObjectAsJson && y >= 0 && y < 1e8) {
				g_preview.restoreScrollY = static_cast<int>(y);
			}
		}
		NavigateQueued();
		return S_OK;
	}

private:
	LONG refCount = 1;
	const ScriptKind kind;
};

// ICoreWebView2NavigationCompletedEventHandler: inject the sync script into
// each freshly loaded page, then bring it to the editor's position (sync on)
// or the reader's previous one (sync off).
class NavigationHandler final : public ICoreWebView2NavigationCompletedEventHandler {
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
		if (riid == IID_IUnknown || riid == IID_ICoreWebView2NavigationCompletedEventHandler) {
			*ppv = this;
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) noexcept override {
		g_preview.pageReady = false;
		// Calls made against the previous page will not be answered usefully.
		g_preview.syncInFlight = false;
		g_preview.syncDirty = false;

		BOOL success = FALSE;
		if (sender == nullptr || args == nullptr || FAILED(args->get_IsSuccess(&success)) || !success) {
			return S_OK;
		}
		// Only our own rendered pages. A link followed out to the web loads a
		// real site in the pane; it gets no script from us.
		LPWSTR source = nullptr;
		bool ours = false;
		if (SUCCEEDED(sender->get_Source(&source)) && source != nullptr) {
			ours = StrCmpNIW(source, L"http:", 5) != 0 && StrCmpNIW(source, L"https:", 6) != 0;
			CoTaskMemFree(source);
		}
		if (!ours) {
			return S_OK;
		}

		if (FAILED(sender->ExecuteScript(SyncScript, nullptr))) {
			return S_OK;
		}
		g_preview.pageReady = true;
		if (bMarkdownPreviewSync) {
			SendEditorState(true);
		} else if (g_preview.restoreScrollY >= 0) {
			WCHAR script[64];
			wsprintf(script, L"window.np4&&np4.restore(%d)", g_preview.restoreScrollY);
			sender->ExecuteScript(script, nullptr);
		}
		return S_OK;
	}

private:
	LONG refCount = 1;
};

// ICoreWebView2FocusChangedEventHandler, used for both GotFocus and LostFocus.
//
// WebView2 hosts its content in a child window whose class is an unspecified
// implementation detail of the runtime, so the pane cannot be recognized by
// inspecting the focused HWND. These events are the documented way to know
// whether the WebView owns input, which the message loop needs so it can stop
// translating accelerators the page should handle itself (Ctrl+C above all).
class FocusHandler final : public ICoreWebView2FocusChangedEventHandler {
public:
	explicit FocusHandler(bool gained) noexcept : focused{gained} {}

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
		if (riid == IID_IUnknown || riid == IID_ICoreWebView2FocusChangedEventHandler) {
			*ppv = this;
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2Controller *sender, IUnknown *args) noexcept override {
		UNREFERENCED_PARAMETER(sender);
		UNREFERENCED_PARAMETER(args);
		g_preview.focused = focused;
		return S_OK;
	}

private:
	LONG refCount = 1;
	const bool focused;
};

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
			// Script runs only to render Mermaid diagrams, and only from the
			// bundled file: the page's CSP forbids inline script entirely, so
			// nothing originating in the document itself can execute. When the
			// asset is missing there is nothing to run and script stays off.
			const bool mermaid = MarkdownPreview_IsMermaidAvailable();

			ICoreWebView2Settings *settings = nullptr;
			if (SUCCEEDED(g_preview.webview->get_Settings(&settings)) && settings != nullptr) {
				// The preview renders local content only: no devtools, no
				// status bar. The context menu stays on because it carries the
				// only discoverable Copy command for the rendered text; its
				// navigation entries are harmless on a page with no links to
				// follow and an opaque origin.
				settings->put_IsScriptEnabled(mermaid ? TRUE : FALSE);
				settings->put_AreDefaultContextMenusEnabled(TRUE);
				settings->put_AreDevToolsEnabled(FALSE);
				settings->put_IsStatusBarEnabled(FALSE);
				settings->put_IsZoomControlEnabled(TRUE);
				settings->Release();
			}

			// Map the asset folder to a virtual host so the page can load
			// mermaid.min.js. NavigateToString() gives the document an opaque
			// origin that cannot reference local files by path, so a mapped
			// host is the only way to serve a bundled script to it.
			WCHAR assets[MAX_PATH];
			if (mermaid && GetAssetFolder(assets, COUNTOF(assets))) {
				ICoreWebView2_3 *webview3 = nullptr;
				if (SUCCEEDED(g_preview.webview->QueryInterface(IID_PPV_ARGS(&webview3))) && webview3 != nullptr) {
					// DenyCors: the script is fetched as a plain <script> tag;
					// nothing needs cross-origin read access to the folder.
					webview3->SetVirtualHostNameToFolderMapping(MERMAID_VIRTUAL_HOST, assets,
						COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
					webview3->Release();
				}
			}
		}

		// Track focus so DispatchMessageMain() can let the page handle its own
		// editing keys. A failure here only costs the accelerator exemption,
		// so it is not worth failing creation over.
		EventRegistrationToken token;
		if (FocusHandler *gotFocus = new (std::nothrow) FocusHandler(true)) {
			controller->add_GotFocus(gotFocus, &token);
			gotFocus->Release();
		}
		if (FocusHandler *lostFocus = new (std::nothrow) FocusHandler(false)) {
			controller->add_LostFocus(lostFocus, &token);
			lostFocus->Release();
		}
		// Sync and scroll preservation both start from a loaded page; without
		// this the preview still works, it just reopens at the top.
		if (g_preview.webview != nullptr) {
			if (NavigationHandler *navigation = new (std::nothrow) NavigationHandler()) {
				g_preview.webview->add_NavigationCompleted(navigation, &token);
				navigation->Release();
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

// Directory holding the bundled web assets: "res" beside the executable.
// Returns false if the path does not fit or the folder is absent.
bool GetAssetFolder(LPWSTR path, DWORD cch) noexcept {
	if (cch < MAX_PATH) {
		return false;
	}
	const DWORD length = GetModuleFileName(nullptr, path, cch);
	if (length == 0 || length >= cch) {
		return false;
	}
	LPWSTR name = PathFindFileName(path);
	if (name == nullptr) {
		return false;
	}
	lstrcpy(name, L"res");
	const DWORD attr = GetFileAttributes(path);
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

} // namespace

bool MarkdownPreview_IsMermaidAvailable() noexcept {
	// Cached: this runs on every render, and the file cannot appear or vanish
	// mid-session in any case that matters.
	static int cached = -1;
	if (cached < 0) {
		WCHAR path[MAX_PATH];
		cached = 0;
		if (GetAssetFolder(path, COUNTOF(path)) && PathAppend(path, L"mermaid.min.js")) {
			const DWORD attr = GetFileAttributes(path);
			if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
				cached = 1;
			}
		}
	}
	return cached > 0;
}

namespace {

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

// Load a rendered page, replacing the current one. Takes ownership of html.
void NavigateNow(char *html) noexcept {
	if (g_preview.webview == nullptr) {
		NP2HeapFree(html);
		return;
	}
	g_preview.pageReady = false;

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

void NavigateQueued() noexcept {
	char *html = g_preview.queuedHtml;
	g_preview.queuedHtml = nullptr;
	if (html != nullptr) {
		NavigateNow(html);
	}
}

// Every render replaces the page, which would put the reader back at the top
// on each refresh -- every pause while typing, with live refresh. With sync on,
// the editor's position is re-applied once the new page loads. With sync off,
// the page is asked where it is first, and the new page is put back there.
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

	if (g_preview.forgetScroll) {
		g_preview.forgetScroll = false;
		g_preview.restoreScrollY = -1;
		if (g_preview.queuedHtml != nullptr) {
			NP2HeapFree(g_preview.queuedHtml);
			g_preview.queuedHtml = nullptr;
		}
		NavigateNow(html);
		return;
	}

	if (bMarkdownPreviewSync || !g_preview.pageReady) {
		// Mid-navigation, the last answer is still the best one available.
		NavigateNow(html);
		return;
	}

	// Keep only the newest render while the query is out.
	if (g_preview.queuedHtml != nullptr) {
		NP2HeapFree(g_preview.queuedHtml);
	}
	g_preview.queuedHtml = html;
	if (g_preview.scrollQueryInFlight) {
		return;
	}
	if (ScriptHandler *handler = new (std::nothrow) ScriptHandler(Script_ScrollQuery)) {
		if (SUCCEEDED(g_preview.webview->ExecuteScript(L"window.scrollY", handler))) {
			g_preview.scrollQueryInFlight = true;
		}
		handler->Release();
	}
	if (!g_preview.scrollQueryInFlight) {
		NavigateQueued();
	}
}

// Send the editor's top line and selection to the page. At most one call is
// in flight; movement in the meantime is folded into a single follow-up, so a
// fast scroll does not queue up a backlog of stale positions.
void SendEditorState(bool force) noexcept {
	if (!bMarkdownPreviewSync || !g_preview.visible || !g_preview.pageReady || g_preview.webview == nullptr) {
		return;
	}
	if (g_preview.syncInFlight) {
		g_preview.syncDirty = true;
		return;
	}

	// Top edge as a document line plus how far into it, for word-wrapped lines
	// that span several display lines; folding is handled by the same mapping.
	const Sci_Line firstDisplay = SciCall_GetFirstVisibleLine();
	const Sci_Line docLine = SciCall_DocLineFromVisible(firstDisplay);
	const Sci_Line docFirstDisplay = SciCall(SCI_VISIBLEFROMDOCLINE, docLine, 0);
	const Sci_Line wraps = max<Sci_Line>(1, SciCall(SCI_WRAPCOUNT, docLine, 0));
	const int frac = static_cast<int>(min<Sci_Line>(999, (firstDisplay - docFirstDisplay) * 1000 / wraps));
	const Sci_Line lineCount = SciCall_GetLineCount();
	const Sci_Line lastDoc = lineCount - 1;
	const Sci_Line totalDisplay = SciCall(SCI_VISIBLEFROMDOCLINE, lastDoc, 0) + SciCall(SCI_WRAPCOUNT, lastDoc, 0);
	const Sci_Line onScreen = SciCall(SCI_LINESONSCREEN, 0, 0);
	// Pin the ends exactly; interpolation alone stops short of both.
	const bool atTop = firstDisplay == 0;
	const bool atBottom = !atTop && firstDisplay + onScreen >= totalDisplay;
	const int flags = (atTop ? 1 : 0) | (atBottom ? 2 : 0);

	int selFrom = -1;
	int selTo = -1;
	if (!SciCall_IsSelectionEmpty()) {
		const Sci_Position selEnd = SciCall_GetSelectionEnd();
		const Sci_Line lineFrom = SciCall_LineFromPosition(SciCall_GetSelectionStart());
		Sci_Line lineTo = SciCall_LineFromPosition(selEnd);
		// Whole lines selected by dragging down end at the start of the next
		// line, which the reader does not think of as selected.
		if (lineTo > lineFrom && selEnd == SciCall_PositionFromLine(lineTo)) {
			--lineTo;
		}
		selFrom = static_cast<int>(lineFrom);
		selTo = static_cast<int>(lineTo);
	}

	const int line = static_cast<int>(docLine);
	if (!force && line == g_preview.sentLine && frac == g_preview.sentFrac && flags == g_preview.sentFlags
		&& selFrom == g_preview.sentSelFrom && selTo == g_preview.sentSelTo) {
		return;
	}

	WCHAR script[160];
	wsprintf(script, L"window.np4&&np4.update(%d,%d/1000,%d,%d,%d,%d,%d)",
		line, frac, atTop ? 1 : 0, atBottom ? 1 : 0, static_cast<int>(lineCount), selFrom, selTo);
	ScriptHandler *handler = new (std::nothrow) ScriptHandler(Script_Sync);
	if (handler == nullptr) {
		return;
	}
	if (SUCCEEDED(g_preview.webview->ExecuteScript(script, handler))) {
		g_preview.syncInFlight = true;
		g_preview.sentLine = line;
		g_preview.sentFrac = frac;
		g_preview.sentFlags = flags;
		g_preview.sentSelFrom = selFrom;
		g_preview.sentSelTo = selTo;
	}
	handler->Release();
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

bool MarkdownPreview_HasFocus() noexcept {
	return g_preview.visible && g_preview.focused;
}

void MarkdownPreview_Toggle(HWND hwndParent) noexcept {
	if (g_preview.visible) {
		g_preview.visible = false;
		g_preview.focused = false;	// hiding does not always raise LostFocus
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

void MarkdownPreview_OnEditorUpdate(unsigned updated) noexcept {
	if (updated & (SC_UPDATE_V_SCROLL | SC_UPDATE_SELECTION)) {
		SendEditorState(false);
	}
}

void MarkdownPreview_ForgetScroll() noexcept {
	g_preview.forgetScroll = true;
}

void MarkdownPreview_Destroy() noexcept {
	if (g_preview.pendingHtml != nullptr) {
		NP2HeapFree(g_preview.pendingHtml);
		g_preview.pendingHtml = nullptr;
	}
	if (g_preview.queuedHtml != nullptr) {
		NP2HeapFree(g_preview.queuedHtml);
		g_preview.queuedHtml = nullptr;
	}
	g_preview.pageReady = false;
	g_preview.syncInFlight = false;
	g_preview.syncDirty = false;
	g_preview.scrollQueryInFlight = false;
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
	g_preview.focused = false;
}

#endif // NP2_ENABLE_MARKDOWN_PREVIEW

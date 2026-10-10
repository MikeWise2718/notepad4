// Test harness for the Markdown preview's source-line markers.
//
// Renders a Markdown file through MarkdownPreview_ToHtml() twice, without and
// with line markers, and writes both. check_markers.py then verifies that the
// markers are pure insertions in valid positions and point at the right lines.
//
// Build and run: tests\markdown-preview\run.ps1
//
// Usage: render_markers <input.md> <plain.html> <marked.html>

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include "config.h"
#include "MarkdownPreview.h"

// Globals MarkdownPreview.cpp reads from Notepad4.cpp. None of them matter to
// rendering; they only have to link.
HWND hwndEdit = nullptr;
int iMarkdownPreviewRefresh = 0;
bool bMarkdownPreviewMermaid = false;
bool bMarkdownPreviewSync = false;

// NP2HeapAlloc() allocates from this; set in main().
HANDLE g_hDefaultHeap = nullptr;

// The editor half of MarkdownPreview.cpp talks to Scintilla. Rendering only
// asks it for colors, and only from paths this harness never calls.
HANDLE g_hScintilla = nullptr;
extern "C" LRESULT __stdcall Scintilla_DirectFunction(HANDLE, UINT, WPARAM, LPARAM) {
	return 0;
}
int MsgBox(UINT, UINT, ...) noexcept {
	return 0;
}

static char *ReadFileBytes(const char *path, size_t *length) {
	FILE *fp = nullptr;
	if (fopen_s(&fp, path, "rb") != 0 || fp == nullptr) {
		return nullptr;
	}
	fseek(fp, 0, SEEK_END);
	const long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	char *data = static_cast<char *>(malloc(static_cast<size_t>(size) + 1));
	if (data != nullptr) {
		*length = fread(data, 1, static_cast<size_t>(size), fp);
		data[*length] = '\0';
	}
	fclose(fp);
	return data;
}

static bool WriteText(const char *path, const char *text) {
	FILE *fp = nullptr;
	if (fopen_s(&fp, path, "wb") != 0 || fp == nullptr) {
		return false;
	}
	fputs(text, fp);
	fclose(fp);
	return true;
}

int main(int argc, char *argv[]) {
	if (argc != 4) {
		fprintf(stderr, "usage: render_markers <input.md> <plain.html> <marked.html>\n");
		return 2;
	}
	g_hDefaultHeap = GetProcessHeap();
	size_t length = 0;
	char *markdown = ReadFileBytes(argv[1], &length);
	if (markdown == nullptr) {
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}

	char *plain = MarkdownPreview_ToHtml(markdown, length, false, false, false);
	char *marked = MarkdownPreview_ToHtml(markdown, length, false, false, true);
	if (plain == nullptr || marked == nullptr) {
		fprintf(stderr, "render failed\n");
		return 1;
	}
	const bool ok = WriteText(argv[2], plain) && WriteText(argv[3], marked);
	return ok ? 0 : 1;
}

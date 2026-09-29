// This file is part of Notepad4.
// See License.txt for details about distribution and modification.
//
// Fork identity for MikeWise2718/notepad4.
//
// Upstream's version (src/Version.h and the generated src/VersionRev.h) is a
// date plus a commit count since a 2013 SVN changeset. It says nothing about
// which fork-local features a binary contains, and because VersionRev.h is
// generated it can silently lag the tree.
//
// This header is tracked, hand-maintained, and carries a version that only
// moves when this fork changes. Bump NP2_FORK_VERSION with every user-visible
// fork change, in the same commit, and add a line to NP2_FORK_CHANGES.
//
// Upstream does not have this file, so it does not conflict on rebase.
#pragma once

// Bumped by hand, one component per kind of change:
//   major - a new pane, window, or other structural addition
//   minor - a new feature or command within an existing surface
//   patch - a fix or refinement to existing fork behavior
#define NP2_FORK_VERSION			L"1.2.0"

// Shown in the title bar, so keep it short.
#define NP2_FORK_NAME				L"mw"

// "mw 1.2.0", the form used in the title bar and About box.
#define NP2_FORK_VERSION_SHORT		NP2_FORK_NAME L" " NP2_FORK_VERSION

// Where this fork lives. The About box has room for the short form only; the
// full URL is what the link actually opens.
#define NP2_FORK_PAGE_DISPLAY		L"MikeWise2718/notepad4"
#define NP2_FORK_PAGE_URL			L"https://github.com/MikeWise2718/notepad4"

// Summary of what this fork adds, shown in the About box. Keep it to what a
// user would notice, newest first; the full history is in git and in
// specs\markdown-preview-pane.md.
#define NP2_FORK_CHANGES \
	L"\x2022 Markdown preview pane (Ctrl+F10), live/idle/manual refresh\r\n" \
	L"\x2022 Mermaid diagrams rendered in the preview, from a bundled copy\r\n" \
	L"\x2022 Preview shown only for Markdown documents\r\n" \
	L"\x2022 Copy and Select All work inside the preview pane"

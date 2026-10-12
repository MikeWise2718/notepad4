# Bug: U+2610 (☐ BALLOT BOX) shows as an empty box instead of the glyph

Reported 2026-10-12 against **mw 1.3.1**. Not yet reproduced or diagnosed in this repo.

## Status

| # | Task | Status |
|---|------|--------|
| 1 | Write up the report | Done |
| 2 | Reproduce with the test file below; note which pane shows the box | todo |
| 3 | Find the cause (see hypotheses) | todo |
| 4 | Fix, and add the test file to the preview's verification document | todo |

## What happened

A Markdown document (`D:\senckenberg\seth\otto\kgalagadi-dt-odd-skeleton.md`, since edited to remove
the character) used **☐ (U+2610 BALLOT BOX)** as a marker, both in running text and alone in table
cells. Viewed in Notepad4, each occurrence appeared as a plain empty rectangle that the user could not
tell apart from a missing-glyph box. The user reported it as "a single square (which means nothing)",
and only later asked whether it was a character the previewer couldn't show. It was read as content,
not as a rendering failure, which is the real cost of this bug.

**Not yet established:** whether the box appeared in the **WebView2 Markdown preview**, the
**Scintilla editor pane**, or both. The user referred to it as "my previewer".

The preview's own verification (`specs/markdown-preview-pane.md`) confirms that ✓ (U+2713), another
3-byte UTF-8 symbol, renders correctly, so the UTF-8 → UTF-16 → `NavigateToString()` path is probably
not at fault in general.

## Test file

Save as UTF-8, open in Notepad4, look at both panes:

```markdown
# Glyph test

Prose: ☐ open, ☑ checked, ☒ crossed, ✓ tick, ✗ cross, □ white square, ▢ rounded square

| Marker | Meaning |
|---|---|
| ☐ | open |
| ✓ | done |
| □ | white square (U+25A1) |

`code: ☐ ✓`
```

Expected: every character renders as its glyph in both panes. Record which ones fail, and where.

## Hypotheses to check

1. **Editor pane, GDI rendering, no font fallback.** With Scintilla's GDI technology, a character
   missing from the editor font (e.g. Consolas has no U+2610) is drawn as a box; DirectWrite
   (`SC_TECHNOLOGY_DIRECTWRITE*`) uses Windows font fallback and finds it in Segoe UI Symbol. Check the
   rendering-technology setting in use and try switching it.
2. **Preview pane, font stack.** The preview body CSS is
   `font-family:'Segoe UI',system-ui,sans-serif` and code is `Consolas,'Courier New',monospace`
   (`src/MarkdownPreview.cpp` ~lines 361, 370, 443). Chromium should fall back to Segoe UI Symbol, but
   check whether the box appears in table cells or code spans only, which would point at a specific
   rule.
3. **Preview pane, the glyph itself.** Segoe UI Symbol's ☐ is an outlined square; at 14 px it may
   simply *look* like a missing-glyph box. If so, the preview is correct and the issue is visual
   ambiguity, not a bug. Compare against ☑ and □ in the same line.

## If it turns out not to be a bug

If hypothesis 3 holds, close this as "renders correctly but is indistinguishable from a missing
glyph", and note the lesson for document authors: avoid ☐ as a marker, since readers can't tell it
from a rendering failure.

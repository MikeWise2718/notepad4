# Vendored md4c

Markdown parser used by the Markdown preview pane
(see [`specs/markdown-preview-pane.md`](../../specs/markdown-preview-pane.md)).

- **Upstream**: https://github.com/mity/md4c
- **Commit**: `c4be8625eb11725d604232b028df10c2ddf9b577` (2026-08-06)
- **License**: MIT (see `LICENSE.md`)

## Files

Vendored unmodified from upstream `src/`:

| File | Purpose |
|---|---|
| `md4c.c` / `md4c.h` | Core CommonMark parser (SAX-like) |
| `md4c-html.c` / `md4c-html.h` | HTML renderer built on the parser |
| `entity.c` / `entity.h` | HTML entity table, used by the renderer |

The upstream CMake and pkg-config files are intentionally not vendored — sources
are compiled directly by the MSVC and MinGW builds.

## Updating

Re-copy the six files from upstream `src/` and update the commit hash above.
Do not edit these files locally; local changes make future updates painful. Any
Notepad4-specific behavior belongs in `src/MarkdownPreview.cpp` instead.

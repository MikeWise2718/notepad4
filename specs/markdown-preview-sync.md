# Markdown preview sync and echo highlight

**Status: Phase 1 implemented and verified (fork 1.3.0). Phase 2 not planned.**

Keep the Markdown preview scrolled to match the editor, and show the editor's
selection as a highlight in the preview. Behind one toggle, off by default.

The four decisions below were taken as proposed: off by default, one toggle,
`Ctrl+Shift+F10`, one-way.

## Task status

| # | Task | Status |
|---|------|--------|
| 0 | Spike: confirm `ExecuteScript` reaches a page under our CSP | Done — works, **even with page script disabled** |
| 1 | Source-line markers in the rendered HTML | Done |
| 2 | Decouple "script enabled" from Mermaid | **Dropped** — not needed, see "What changed from the plan" |
| 3 | Sync script (host-injected rather than nonce'd in the page) | Done |
| 4 | Keep scroll position across refreshes | Done |
| 5 | Editor → preview scroll sync | Done |
| 6 | Editor selection → preview echo highlight | Done |
| 7 | Toggle: menu item, shortcut, persisted setting | Done |
| 8 | Fork version 1.2.0 → 1.3.0, About change list, spec notes | Done |
| 9 | Preview → editor sync (reverse direction) | **Not planned** — see Phase 2 |

## What changed from the plan

**The sync script is injected by the host, not shipped in the page.** WebView2
runs `ExecuteScript()` code even when page script is disabled, and the page's
CSP does not apply to it. The spike confirmed both, with the Mermaid asset
removed so page script was off. So the sync script lives in
`MarkdownPreview.cpp` as `SyncScript`, is injected after each
`NavigationCompleted`, and page script stays tied to Mermaid exactly as before.
That is strictly safer than the plan, which would have turned page script on
whenever sync was on. Task 2 was dropped as a result.

Links followed out of the preview to `http(s)` pages do not get the script.

**Every document switch forgets the scroll position.** Without that, a newly
opened file's preview would open at the previous file's position. It happens in
`UpdateMarkdownPreviewForDocument()`, which already runs on load, on new, and on
a scheme change.

## Results

Automated, repeatable: `tests/markdown-preview/run.ps1` builds a console
harness around `MarkdownPreview_ToHtml()` and checks each sample in
`tests/markdown-preview/samples/`, with LF, CRLF and CR-only variants. A Python
checker (a different parser from the C++ one) verifies that markers are pure
insertions, that each parses as a real element (never inside a tag, an
attribute, `<style>` or `<textarea>`), and that the text after each marker is on
that source line. A mutation test confirmed the checker catches each failure
kind, including off-by-one line labels.

In the app, driven only by messages to the Scintilla window plus screen capture:

| Check | Result |
|---|---|
| Scroll sync, page script on | Editor top line 162 (`## Section 31`) ↔ preview "Section 31" at top |
| Scroll sync, page script **off** | Line 222 ↔ "Section 40" at top |
| Echo highlight | Selecting lines 169–170 highlights exactly that paragraph |
| Mermaid block | Preview interpolates part-way into the diagram |
| Bottom pin | Both panes end on Section 60 |
| Sync off: preservation | Preview stayed at the end through two full re-renders while the editor sat at the top |

**Performance:** both renders (plain and marked) of a 20,000-line document take
28 ms in total; all 12,500 non-blank lines get a correct marker.

**Cost:** markers make the HTML about 66% larger (520 KB → 863 KB on that
document). The preview's existing 1.5 MB cap on what it can display therefore
arrives sooner with sync on — at roughly 600 KB of Markdown instead of about
1 MB. Beyond the cap the preview is truncated, as before.

**Not verified by me:** the `Ctrl+Shift+F10` keystroke itself (the command was
driven by its ID; pressing keys into whatever window is in front is off-limits
after an earlier incident), and the feel of scrolling a real document by hand.

## What you get

- **View → Markdown Preview → Sync with Editor** (`Ctrl+Shift+F10`), off by default.
- With it on, scrolling the editor scrolls the preview so the same source line is
  at the top of both panes.
- With it on, selecting text in the editor highlights the corresponding lines in
  the preview. An empty selection clears the highlight.
- **Regardless of the toggle:** the preview stops jumping back to the top every
  time it refreshes. Today every live refresh while typing — every 300 ms of
  quiet — reloads the page at scroll position 0. That is a bug in its own right
  and this work fixes it as a side effect (task 4).

## Decisions I need from you

1. **Off by default?** You said "sometimes", so I've assumed off. Easy to flip.
2. **One toggle for both scroll and highlight?** I've assumed yes. Two toggles for
   one pane is a lot of menu; say if you want them separate.
3. **Shortcut `Ctrl+Shift+F10`?** It's free and sits next to the pane's own
   `Ctrl+F10`. (`Shift+F10` is Windows' context-menu key, so not that.)
4. **One-way only for now?** Editor drives preview. Scrolling the *preview* does
   not move the editor. See Phase 2 for why I'd defer the reverse.

## Design

The design as planned. Sections 2 and 3 were superseded during implementation;
see "What changed from the plan" above.

### 1. Where source positions come from (no changes to md4c)

The hard part of any preview sync is knowing which source line produced which
piece of rendered HTML. md4c does not report line numbers, and
`src/md4c/README-vendored.md` forbids editing the vendored files.

It turns out not to be needed. md4c's HTML renderer passes unescaped text to our
output callback as **pointers into the original Markdown buffer**
(`render_html_escaped()` calls `render_verbatim(r, data + beg, …)`), while tags
arrive as string literals. So `HtmlBuffer`'s output callback in
`MarkdownPreview.cpp` can tell, for every text chunk, exactly where in the source
it came from: `chunk - input`, when the pointer lies inside the input.

Whenever the source **line** of the text being emitted changes, the callback
inserts an empty marker before it:

```html
<p><span data-l="12"></span>First line of a paragraph,
<span data-l="13"></span>a second line.</p>
```

This gives **per-line** precision, including inside paragraphs and code blocks —
better than the usual one-marker-per-block approach. Offset → line is a binary
search over a line-start table built once per render. Cost: about 20 bytes per
source line, well inside the existing 1.5 MB cap.

**Where a marker must not go**, and how that is avoided:

- *Inside a tag or attribute* — image alt text is escaped text that lands inside
  `alt="…"`, and raw HTML can be split across lines mid-tag. The callback tracks
  a tiny state machine over everything it emits (in-content / in-tag / in-quote)
  and only inserts in content. This is reliable because md4c escapes `<`, `>` and
  `"` in all text it generates.
- *Inside raw `<style>`, `<textarea>`, `<title>`* in hand-written HTML — a marker
  there would show up as literal text. The state machine also tracks these
  raw-text elements and suppresses markers inside them.
- Chunks that are *not* in the input buffer (entities, `<br>`, synthesized
  strings) are ignored by the bounds check, so they can never produce a wrong line.

Markers are emitted only when sync is on, so the default output is byte-for-byte
what it is today.

### 2. Script and the security boundary

Sync needs JavaScript in the page. Today script is enabled only when Mermaid is
available (`put_IsScriptEnabled(mermaid ? TRUE : FALSE)`), so turning Mermaid off
would silently break sync.

Proposal: **enable script whenever either feature wants it.** This does not widen
the boundary, because the page's CSP is what actually protects you — it already
forbids any script that lacks the per-render nonce, so nothing in the Markdown
file can execute. The sync script is ours, inline, carrying that nonce, exactly
like the existing Mermaid bootstrap.

`IsScriptEnabled` only takes effect on the next navigation, which is fine since
every render is a navigation.

### 3. Talking to the page: host → page only

The page-side script defines a small API, e.g. `window.np4.scrollToLine(line, frac)`
and `window.np4.highlightLines(from, to)`. The host calls it with
`ICoreWebView2::ExecuteScript`, a host-initiated call. Nothing flows from the page
back to the application, so there is still no channel a document could use to
reach Notepad4.

**Task 0 is a spike** to confirm `ExecuteScript` works under our CSP (`script-src`
restricted to the virtual host plus a nonce). I believe host-injected script is
exempt from page CSP, as it is in Chromium DevTools, but this is the assumption
the design rests on, so it gets checked before anything else is built. Fallback
if wrong: drive the page through `PostWebMessageAsJson` instead, which the
nonce'd script listens for — still one-way, still host → page.

### 4. Not losing your place on refresh

Every refresh is a full `NavigateToString()`, which resets scroll. Fix:

- **Sync on:** after `NavigationCompleted`, re-sync from the editor's current
  position. The editor is the source of truth, so nothing needs remembering.
- **Sync off:** before navigating, read `scrollY` with `ExecuteScript` and pass it
  into the next render, where the nonce'd script restores it on load. Async, but
  refreshes are already debounced.

### 5. Editor → preview scroll

On `SCN_UPDATEUI` with `SC_UPDATE_V_SCROLL` (already handled at
`Notepad4.cpp` ~4930):

- Top visible **document** line: `SCI_GETFIRSTVISIBLELINE` → `SCI_DOCLINEFROMVISIBLE`,
  so word wrap and folding are handled. Plus a fraction for how far into a
  wrapped line the top edge sits.
- Page script finds the markers at or before and after that line and
  **interpolates** between their positions, then scrolls so that point is at the
  top. Interpolation is what keeps it smooth: one short source line can render as
  a tall table or diagram, and snapping block-to-block feels jumpy.
- Coalesced to one call per frame, so fast scrolling doesn't queue up scripts.

### 6. Echo highlight

On `SCN_UPDATEUI` with `SC_UPDATE_SELECTION`: send the selection's first and last
**line**. The page builds a DOM `Range` from the marker for the first line to the
marker for the line after the last, and paints it with the CSS Custom Highlight
API (`CSS.highlights` / `::highlight()`).

That API matters: it paints without touching the DOM, so highlighting can't shift
layout, break Mermaid's SVG, or fight the next refresh. WebView2's evergreen
runtime has supported it since Chromium 105.

**Precision is line-level, deliberately.** Mapping a column inside a source line
to a position in rendered text means undoing `**bold**`, `[links](…)`, entities
and so on — guesswork. Line-level is honest and covers the real use: "where is
this in the preview?" Selecting part of a line highlights that whole rendered
line.

Theme-aware: a translucent highlight colour in both light and dark preview
themes.

### 7. The toggle

Mirrors `MarkdownPreviewMermaid` exactly:

- `bool bMarkdownPreviewSync`, INI key `MarkdownPreviewSync`, default `false`,
  saved with `SetBoolEx` so the default isn't written out.
- `IDM_MARKDOWN_PREVIEW_SYNC` (41006) in the preview submenu, checked/enabled
  under the same `available && markdown` rule as the other items.
- `Ctrl+Shift+F10` in the accelerator table.
- Turning it on triggers a refresh (markers are only emitted while it's on).
  Turning it off clears the highlight and leaves the preview where it is.

## Phase 2 (not planned): preview → editor

Scrolling the preview to move the editor sounds symmetric but isn't:

- It needs a **page → application channel** (`WebMessageReceived`). Low risk under
  this CSP — only our nonce'd script can post — but it is the first time the page
  would be able to reach Notepad4 at all, and it would need a strict message
  schema.
- It needs **loop suppression**: editor scroll → preview scroll → editor scroll.
  Doable, but a classic source of jitter.

My suggestion is to live with one-way sync for a while. If you find yourself
wanting to steer from the preview, it's a contained follow-up.

## Risks

- **`ExecuteScript` vs CSP** — the design's one unverified assumption; task 0.
- **Large documents** — one marker per line adds DOM nodes. A 10,000-line file
  adds 10,000 empty spans; Chromium handles that, but I'll measure on the largest
  Markdown file you have before calling it done.
- **Mermaid diagrams** are tall blocks from a single fenced source; scroll
  interpolation across them will move quickly through the source lines. Correct,
  but it will feel different from text.
- **Upstream divergence** — almost all of this lives in `MarkdownPreview.cpp`,
  which upstream doesn't have. The touches to shared files are the
  `SCN_UPDATEUI` hook, one menu item, one accelerator and one setting.

## Verification

Lessons from earlier sessions apply: no synthetic keystrokes or clicks into
whatever window is in front, and no `PrintWindow` for WebView2 content.

- **Markers:** render test documents (paragraphs, wrapped lines, lists, nested
  quotes, tables, code blocks, images with alt text, raw HTML split across lines,
  `<style>` blocks, Mermaid) to HTML and check every marker lands in content
  with the right line. This is plain text inspection, no UI involved.
- **Scroll sync:** drive the editor with `SCI_SETFIRSTVISIBLELINE` sent directly
  to the Scintilla window. That targets a specific HWND, not the focused window.
  Then capture with `CopyFromScreen` (DPI-corrected) and check the preview.
- **Highlight:** `SCI_SETSEL` the same way, then capture.
- **You** try it on real documents before I call it done. Feel is the thing
  automated checks can't judge.
- Builds: x64 Release verified; ARM64 and MinGW remain unverifiable here, as for
  the rest of the preview.

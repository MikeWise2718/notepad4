"""Check the Markdown preview's source-line markers.

    python check_markers.py <source.md> <plain.html> <marked.html>

<plain.html> and <marked.html> are the same document rendered without and with
line markers (see render_markers.cpp). Three properties are checked, using a
different parser from the one in MarkdownPreview.cpp so a shared mistake is
unlikely:

1. Pure insertion: removing the markers gives back the plain render exactly.
2. Valid position: every marker parses as a real <span> element. None lands
   inside a tag, an attribute value, or a raw-text element (<style>,
   <textarea>, ...), where it would be inert text or corrupt the markup.
3. Right line: the text that follows a marker is found on that source line.

Standard library only, so it runs from any Python.
"""

import html
import re
import sys
from html.parser import HTMLParser

MARKER = re.compile(r'<span data-l="(\d+)"></span>')
RAW_TEXT = ('script', 'style', 'textarea', 'title', 'xmp', 'iframe', 'noembed', 'noframes', 'noscript')


class SpanCounter(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.markers = 0
        self.bad_attrs = []

    def handle_starttag(self, tag, attrs):
        names = [name for name, _ in attrs]
        if tag == 'span' and names == ['data-l']:
            self.markers += 1
            return
        for name, value in attrs:
            if 'data-l' in name or (value and 'data-l=' in value):
                self.bad_attrs.append((tag, name, value))


def source_lines(text):
    # Same rule as Scintilla and MarkdownPreview.cpp: CRLF, LF, or a lone CR.
    return re.split(r'\r\n|\n|\r', text)


def text_after(marked, end):
    """Plain text from a marker up to the next tag, for the line check."""
    stop = marked.find('<', end)
    snippet = marked[end:stop if stop >= 0 else len(marked)]
    return html.unescape(snippet).strip()


def check(source_path, plain_path, marked_path):
    with open(source_path, encoding='utf-8', newline='') as f:
        source = f.read()
    with open(plain_path, encoding='utf-8', newline='') as f:
        plain = f.read()
    with open(marked_path, encoding='utf-8', newline='') as f:
        marked = f.read()

    name = source_path.replace('\\', '/').rsplit('/', 1)[-1]
    errors = []
    matches = list(MARKER.finditer(marked))

    # 1. Pure insertion. The body only: with markers on, <head> also carries the
    # selection highlight style.
    def body(page):
        return page[page.find('<body>'):]
    if body(MARKER.sub('', marked)) != body(plain):
        errors.append('removing the markers does not give back the plain render')

    # 2. Valid position.
    counter = SpanCounter()
    counter.feed(marked)
    counter.close()
    if counter.markers != len(matches):
        errors.append(f'{len(matches)} markers in the text but {counter.markers} parse as elements '
                      '(the rest are inside a tag or raw-text element)')
    for tag, attr, value in counter.bad_attrs:
        errors.append(f'marker text inside <{tag} {attr}="{value}">')
    for element in RAW_TEXT:
        for region in re.finditer(rf'<{element}\b[^>]*>(.*?)</{element}', marked, re.I | re.S):
            if MARKER.search(region.group(1)):
                errors.append(f'marker inside <{element}>')

    # 3. Right line. Backslash escapes are dropped from the source, since the
    # render shows the escaped character without them.
    lines = source_lines(source)
    wrong = 0
    checked = 0
    for match in matches:
        line = int(match.group(1))
        if line >= len(lines):
            errors.append(f'marker for line {line + 1}, but the source has {len(lines)} lines')
            continue
        snippet = text_after(marked, match.end())[:16]
        if not snippet:
            continue
        checked += 1
        if snippet not in re.sub(r'\\(.)', r'\1', lines[line]):
            wrong += 1
            if wrong <= 5:
                errors.append(f'line {line + 1}: rendered text {snippet!r} is not on that source line: '
                              f'{lines[line][:60]!r}')
    if wrong > 5:
        errors.append(f'... {wrong - 5} more wrong-line markers')

    marked_lines = {int(m.group(1)) for m in matches}
    content_lines = sum(1 for text in lines if text.strip())
    status = 'FAIL' if errors else 'ok'
    print(f'{status:4} {name}: {len(matches)} markers on {len(marked_lines)} of {content_lines} '
          f'non-blank lines, {checked} line checks')
    for error in errors:
        print(f'     {error}')
    return not errors


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    sys.exit(0 if check(*sys.argv[1:]) else 1)

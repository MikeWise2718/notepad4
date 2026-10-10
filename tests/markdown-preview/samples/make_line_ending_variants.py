"""Regenerate constructs-crlf.md and constructs-cr.md from constructs.md.

The marker code numbers lines the way Scintilla does: CRLF, LF and a lone CR
each end a line. These variants check that, and are kept byte-exact in git by
.gitattributes (-text) so autocrlf cannot normalize them.
"""

from pathlib import Path

here = Path(__file__).parent
text = (here / 'constructs.md').read_bytes().replace(b'\r\n', b'\n')
(here / 'constructs-crlf.md').write_bytes(text.replace(b'\n', b'\r\n'))
(here / 'constructs-cr.md').write_bytes(text.replace(b'\n', b'\r'))

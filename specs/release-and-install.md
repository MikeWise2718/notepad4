# Release and install without compiling

Get this fork onto other Windows machines without a build environment: a
portable zip published as a GitHub Release, and a PowerShell script that
installs or updates from it.

## Task status

| # | Task | Status |
|---|------|--------|
| 1 | `tools/release.ps1`: build, package, publish a GitHub Release | Done |
| 2 | `tools/install.ps1`: install, update in place, uninstall | Done |
| 3 | Local tests: fresh install, update while running, settings kept, uninstall | Done |
| 4 | First release published (`mw-1.3.1`) | See below |
| 5 | Install tested on a second machine | Not done — needs you, or permission to use syther |
| 6 | Build the release in CI instead of on spearow | Not planned |

## Using it

**Publish a release** (on spearow, after committing and pushing):

```powershell
powershell -File tools\release.ps1          # build, zip, publish
powershell -File tools\release.ps1 -DryRun  # build and zip only, into build\bin\release
```

Bump `NP2_FORK_VERSION` in `src\ForkVersion.h` first; the script refuses to
reuse a tag. It also refuses a dirty tree or a HEAD that is not `origin/main`,
so every release corresponds to a commit anyone can check out.

**Install or update** on any Windows 10/11 x64 machine, no admin:

```powershell
irm https://raw.githubusercontent.com/MikeWise2718/notepad4/main/tools/install.ps1 | iex
```

Options, when run as a file (`powershell -ExecutionPolicy Bypass -File install.ps1 ...`):
`-InstallDir`, `-Version 1.3.1`, `-ZipPath <local zip>`, `-NoPath`, `-NoShortcut`,
`-Uninstall`.

## Design

**Portable, per user.** Notepad4 is already portable, so the install is just
files in `%LOCALAPPDATA%\Programs\Notepad4`, a Start menu shortcut and a user
PATH entry. No installer toolchain, no admin, no registry beyond PATH.

**What ships** — upstream's `make_zip.bat` layout plus the preview's files:

| File | Why |
|---|---|
| `Notepad4.exe`, `matepath.exe` | the editor, and the file browser behind its Browse button |
| `WebView2Loader.dll`, `res\mermaid.min.js` | Markdown preview and its diagrams |
| `Notepad4.ini-default`, `Notepad4 DarkTheme.ini-default`, `matepath.ini-default` | seed settings |
| `License.txt`, `VERSION.txt` | licence; tag and commit, read by the installer |

**Settings are never overwritten.** In a portable install `Notepad4.ini` lives
beside the exe. Notepad4 itself creates it on first run, copying
`Notepad4.ini-default` if present (`FindIniFile()` in `src\Notepad4.cpp`).
Releases contain only the `-default` files, so an update cannot touch real
settings, and `-Uninstall` keeps them.

**Updating while Notepad4 is open.** A running exe cannot be overwritten but
can be renamed. The installer moves locked files to `*.old`, puts the new ones
in place, and deletes the `.old` files on its next run. Open windows keep the
old version until closed.

**Integrity.** Each release carries a `.sha256`; the installer checks it, for
downloads and for `-ZipPath`.

**Windows PowerShell 5.1** is the target for the installer, since it is what
every Windows machine has. Two 5.1 pitfalls are avoided in the release script:
`Compress-Archive` writes backslashes into zip entry names (the zip is written
with `System.IO.Compression` instead), and redirecting a native command's
stderr under `ErrorActionPreference=Stop` can throw (so the tag check lists
releases instead of probing for one).

**WebView2 Runtime.** Needed for the preview; Windows 11 includes it, Windows
10 usually has it through Edge. The installer checks its registry entry and
says so if it is missing; the editor works either way.

**Repository.** Releases go to `MikeWise2718/notepad4`, named explicitly: `gh`
on spearow defaults to the upstream repo for some commands. The fork is public,
so releases are publicly downloadable, like the source.

## Verification

Run on spearow against a scratch install folder with `-NoPath -NoShortcut`, so
the real PATH and Start menu were not touched:

| Test | Result |
|---|---|
| Fresh install from the local zip | All 9 files in place, checksum verified, WebView2 found |
| Reinstall with the installed Notepad4 open | Exe and DLL moved to `.old`; window kept running; new files in place |
| Settings across the update | `Notepad4.ini` (with a marker line) byte-identical |
| Re-run after closing | `.old` files removed |
| `-Uninstall` | Program files removed; `Notepad4.ini` and `Notepad4 DarkTheme.ini` kept, marker intact |
| `irm | iex` with a `param()` block, in 5.1 | Defaults apply, `return` exits cleanly |

Not done here: an install on a second, clean machine. syther would be the
natural one, but installing software there was not part of the request.

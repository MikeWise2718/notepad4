# Fork versioning

How this fork identifies itself, and why the build stamps a version at all.

## Task status

| # | Task | Status |
|---|------|--------|
| 1 | `src/ForkVersion.h`: tracked, hand-maintained fork version | Done |
| 2 | Fork version in the window title | Done |
| 3 | Fork section in the About box (version, changes, repo link) | Done |
| 4 | `StampVersionRev` MSBuild target runs `version.sh` automatically | Done |
| 5 | `skip-worktree` on the two generated-but-tracked files | Done (per clone) |
| 6 | Same treatment for the MinGW build | Not done |
| 7 | Same treatment for matepath | Not done |

## The problem

Two separate things were wrong.

**Upstream's version was stale.** `src/Version.h` is tracked; the numbers live in
`src/VersionRev.h`, which `version.sh` generates from git: `VERSION_MINOR`/`BUILD`
are just `date +%y`/`+%m`, and `VERSION_REV` is a commit count since a 2013 SVN
changeset. Nothing invoked `version.sh`, so the header held whatever upstream last
committed at release time. Builds here reported **26.07.6234 / `5c3dca05`** -- a
commit 66 behind HEAD, from before every fork feature existed, in the wrong month.

**Upstream's version cannot describe this fork anyway.** A commit count since an
SVN changeset says nothing about which fork-local features a binary has. Two builds
with the same upstream revision can differ entirely.

## The fix

### `src/ForkVersion.h` (tracked, hand-maintained)

Carries `NP2_FORK_VERSION`, a short fork name, the repo URL, and
`NP2_FORK_CHANGES` -- the user-visible feature list shown in About.

**Bump it in the same commit as any user-visible fork change**, and add a line to
`NP2_FORK_CHANGES`. Nothing enforces this; it is the one manual step.

Upstream has no such file, so it never conflicts on rebase.

### Where the version shows up

- **Window title** -- `file.md [dir] - Notepad4 (mw 1.2.0)`. Appended in
  `UpdateWindowTitle()`. `szTitle` is `WCHAR[512]` and was already near its worst
  case (~425 chars) before this; the addition is ~11 more. Watch that bound if
  anything else is ever appended.
- **About box** -- a divider, `Fork: mw 1.2.0`, a read-only multiline edit with the
  change list, and a link to this repo. `IDD_ABOUT` grew from 144 to 226 DLU tall.
- **About > Copy** -- the pasteable build info now has a `Fork:` line, so a bug
  report identifies the fork build.

### `StampVersionRev` (MSBuild target)

Runs `version.sh` via git-bash before `ClCompile`, so `VersionRev.h` is always
stamped from the real checkout. Deliberately fail-soft: no git-bash, no git, or a
source tarball still builds, with a warning rather than an error. `version.sh`
rewrites the header only when the content changes, so this does not force a
rebuild every time.

## Gotcha: two tracked files that the build rewrites

`version.sh` writes `src/VersionRev.h` **and** `res/Notepad4.exe.manifest`, and
upstream tracks both -- it commits them at release time ("Release 26.07r6234").
Automating the stamp therefore dirties two tracked files on every build, which
buries real changes in `git status` and invites committing build noise.

Resolved with `skip-worktree`, which keeps them tracked at upstream content while
ignoring local modifications:

```sh
git update-index --skip-worktree src/VersionRev.h res/Notepad4.exe.manifest
```

**This is per-clone local state and does not travel with the repo.** A fresh clone
must run it again, or the first build will show two spurious modified files. To
undo (e.g. to take an upstream update to these files):

```sh
git update-index --no-skip-worktree src/VersionRev.h res/Notepad4.exe.manifest
```

Gitignoring them instead was rejected: they are tracked upstream, and untracking
would diverge the fork and complicate every rebase.

## Not done

- **MinGW build** does not run the stamp target -- that is MSBuild-only. A MinGW
  build still gets `ForkVersion.h` (it is a plain header) but keeps whatever
  `VersionRev.h` happens to be on disk.
- **matepath** is unchanged. `version.sh` updates it only when passed an argument,
  which the target does not do.
- **About box still points bug reports at upstream.** `HELP_LINK_REPORT_ISSUE` and
  friends in `Version.h` go to `zufuliu/notepad4`; fork bugs do not belong there.
  Changing them means editing a tracked upstream file, so it was left alone
  pending a decision.

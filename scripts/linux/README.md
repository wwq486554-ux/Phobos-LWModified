# Building Phobos on Linux (without rebooting into Windows)

> **This script is an addition of the Phobos-LWModified fork**, not part of
> upstream Phobos — upstream builds with MSBuild on Windows (see `scripts/*.bat`).
> This fork-added path builds the very same sources on Linux instead, by running
> the real MSVC toolchain under Wine.

These scripts build `Phobos.dll` on Linux by running the **real MSVC toolchain**
(`cl.exe` / `rc.exe` / `link.exe`) that is already installed on the Windows
volume, under Wine. Nothing is downloaded and no second compiler is introduced,
so the result is produced by the same toolset with the same options as a
Visual Studio build.

The compiler / resource-compiler / linker command lines are mirrored from the
`CL.command.1.tlog`, `rc.command.1.tlog` and `link.command.1.tlog` files that
MSBuild writes into `Release/IntDir/Phobos.tlog/`, i.e. from a real Visual
Studio build of this project.

## Requirements

* Wine (`wine` in `PATH`). 32-bit support is not needed for the compiler
  itself; the SDK's `rc.exe` is 32-bit and works through Wine's WoW64.
* A **mounted** Windows volume containing Visual Studio's C++ tools and the
  Windows 10/11 SDK (read-only mount is fine, e.g. `/mnt/Windows-SSD`).
* The Wine prefix must live on a **writable Linux-native filesystem** (ext4,
  btrfs, xfs, tmpfs...). Two setups both surface as a compiler error that looks
  unrelated to the environment:
  * an NTFS/FUSE mount -> `error D8037: cannot create temporary il file`;
  * a **read-only** mount -> the same `D8037`, or
    `error D8050: cannot execute c1xx.dll`, because `cl.exe` cannot create its
    temp files at all.

  Sources, object files and build output may all stay on NTFS - only the prefix
  (and its temp directory) must be native and writable. When `$HOME/.cache` is
  not writable the default prefix falls back to `<parent of the repo>/.wine-pbsnew`,
  so no environment setup is needed in a sandbox with a read-only root.
* `YRpp` submodule initialised: `git submodule update --init --recursive`.

## Quick start

```bash
# once - creates the prefix ($HOME/.cache/pbsnew-wine, or a workspace-local
# fallback when $HOME is read-only) and verifies that cl.exe runs
scripts/linux/setup_wineprefix.sh

# every build
scripts/linux/build_linux.sh Release
```

The output is written where the Visual Studio build puts it:

```
Release/Phobos.dll
Release/Phobos.pdb
```

## Usage

```
scripts/linux/build_linux.sh [Debug|Release] [options]

  --build-type NIGHTLY|RELEASE   defines NIGHTLY / RELEASE (default: none)
  -j, --jobs N                   parallel compiler invocations (default: nproc)
  --clean                        drop all Linux-side build state and rebuild
  --fix-eol                      normalise repository line endings, then build
  --deploy DIR                   copy Phobos.dll / Phobos.pdb / gamemd.edb into DIR
                                 (existing files are kept as *.prev first)
  --stage                        also copy gamemd.edb (and Syringe.exe)
  -v, --verbose                  echo compiler commands and warnings
  --msvc-ver VER                 MSVC toolset (default 14.44.35207, the v143 one)
  --sdk-ver VER                  Windows SDK version (default: newest installed)
  --prefix PATH                  Wine prefix (default: $HOME/.cache/pbsnew-wine,
                                 falling back to <repo>/../.wine-pbsnew when
                                 $HOME is not writable)
```

Useful environment variables: `PBS_WINDOWS_ROOT` (when the Windows volume is
not auto-detected), `PBS_WINEPREFIX`, `PBS_MSVC_VER`, `PBS_SDK_VER`,
`PBS_JOBS`, `PBS_SYRINGE_EXE` (used by `--stage`), `PBS_DEPLOY_DIR` (default
game directory for `--deploy`), and `PBS_WINE_LOCALE`
(see "Localised compiler messages" below).

## Build, deploy, play

Building and copying the result into the game directory is one command:

```bash
scripts/linux/build_linux.sh Release --deploy "/path/to/your-mod"
```

`--deploy DIR` copies `Phobos.dll`, `Phobos.pdb` and `gamemd.edb` into `DIR`,
keeping whatever was there as `*.prev` (that is your rollback). It refuses to
run if `DIR` does not exist, warns if `DIR` has no `gamemd.exe`, and copies
nothing when the file is already identical. Set `PBS_DEPLOY_DIR` once (e.g. in
`~/.bashrc`) and a plain `scripts/linux/build_linux.sh Release` builds and
deploys in one go.

Then start the game through `SyringeEx.exe` (or the mod's usual launcher). If
something goes wrong, look at:

* the newest folder under `debug/` in the game directory plus `debug.log` for
  crash dumps - copy `Phobos.pdb` next to the DLL so dumps can be symbolised;
* `syringe.log` for "DLL failed to load"/hook registration problems;
* `Release/IntDir/linux/log/` for compiler/linker output;
* and rename `Phobos.dll.prev` back to `Phobos.dll` to roll back.

## Errors and warnings

A failing build is loud and exits non-zero:

* A translation unit that does not compile is listed by name, followed by its
  `cl.exe` output (last 25 lines) and the count of failures; the build then
  aborts with exit code 1. Successfully compiled units are kept, so fixing one
  error only recompiles that file.
* A failed `rc.exe` or `link.exe` prints its log and aborts the same way.
* Warnings are printed too, in the log's own language.
* Everything is also kept under `Release/IntDir/linux/log/` (`*.log` per unit,
  `link.log`, `rc_*.log`), so you can grep the full text afterwards.
* A source listed in `Phobos.vcxproj` but missing on disk, an uninitialised
  Wine prefix, a prefix on an unsuitable filesystem, or a missing toolchain all
  abort with an explicit message before any compiling starts.

### Localised compiler messages

Wine picks its Windows codepages from `LC_ALL` (not `LANG`), and a Chinese
Visual Studio install writes CP936 messages. With a non-Chinese `LC_ALL` every
non-ASCII character is already replaced by `?` when the log is written, so the
text cannot be recovered afterwards - only error codes, file names and line
numbers survive. `common.sh` therefore sets `LC_ALL=zh_CN.UTF-8` automatically
when the host has that locale (override with `PBS_WINE_LOCALE`), and
`build_linux.sh` decodes the resulting GBK logs to UTF-8 for display. Setup
warns when the tools are localised but no suitable locale exists.

A nightly-equivalent build:

```bash
scripts/linux/build_linux.sh Release --build-type NIGHTLY
```

## How it works

1. `common.sh` locates the Visual Studio installation and the Windows SDK on the
   mounted Windows volume and builds the `INCLUDE` / `LIB` lists for the tools.
2. `build_linux.sh` reproduces `Phobos.props`' `ComputeGitInfo` target: it
   writes `Release/IntDir/Generated/Phobos.Git.h` with the commit / ref / dirty
   stamp taken from the local repository.
3. The source list is read from `Phobos.vcxproj` plus `YRpp/YRpp.props`, so it
   always matches the Visual Studio project (currently 234 translation units
   and 2 resource files).
4. Translation units are compiled in parallel (`wine cl.exe /c ... /Z7`), then
   the resources are compiled (`wine rc.exe`), then everything is linked
   (`wine link.exe /DLL /MACHINE:X86 /DEBUG:FULL ...`).
5. Build state lives in `Release/IntDir/linux/` so MSBuild's own incremental
   state in `Release/IntDir/` is left untouched.

Incremental builds are conservative: a translation unit is recompiled when its
`.cpp` is newer than its `.obj`, and **every** unit is recompiled when any
header, `.inl` or `.rc` under `src/` or `YRpp/` changed or when the compiler
options (or the MSVC/SDK version) changed. (Object-level dependency files from
`/showIncludes` are not parsed yet.) `Debug` and `Release` keep separate object
trees, so switching between them does not rebuild the other configuration.

### Measured on the development machine (28 cores)

| Build | Time |
|---|---|
| Full `Release` build (234 units, `-j 28`) | ~95-100 s |
| Full `Debug` build | ~96 s |
| One `.cpp` touched | ~4 s (recompile + relink) |
| Nothing changed | instant |

A typical full build spends most of its time inside MSVC itself (about 18
minutes of CPU time), not in Wine.

## Verifying the output

```
file Release/Phobos.dll                      # PE32 executable ... Intel i386
llvm-readobj --sections Release/Phobos.dll   # must contain .syhks00, .patch, .fptable
llvm-readobj --coff-exports Release/Phobos.dll | grep -c 'Name:'   # ~1492 hooks
```

A successful build of the current tree produces a ~1.8 MB `Release/Phobos.dll`,
a ~34 MB `Release/Phobos.pdb`, and no linker warnings.

## Known deviations from the Visual Studio build

These do not change generated code, but they are deliberate:

* `/Z7` is used instead of `/Zi` by default: debug info goes into the object
  files and `link.exe` still produces the same `Phobos.pdb`, but the compiler
  no longer needs the shared `mspdbsrv` PDB server, which is fragile when many
  `cl.exe` processes run under Wine. Set `PBS_DEBUG_INFO=zi` to match MSBuild
  exactly.
* `/experimental:module` is not passed by default (the project builds with no
  C++ modules; MSBuild does pass it). Set `PBS_MODULES=1` to re-enable it.
* `Debug` uses `/Zi` rather than MSBuild's edit-and-continue default, and never
  enables `/RTC1` (this project sets `UseDebugLibraries=false`, so MSBuild does
  not enable it either).
* No PCH and no custom build steps exist in this project, so nothing else has
  to be emulated.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `Wine prefix '...' is not initialised - run setup_wineprefix.sh first` | Expected on a fresh checkout: the build refuses to run MSVC in a prefix `wineboot` has not created. Run `scripts/linux/setup_wineprefix.sh` once (it also initialises a leftover empty prefix directory). |
| `error D8037: cannot create temporary il file` | Wine prefix (or `TMP`) is on NTFS/FUSE, is on a **read-only** mount, or its `system.reg` is truncated (`wineboot` did not finish - a healthy one is ~3.4 MB). Move it to a writable native filesystem, or `rm -rf <prefix>` and re-run `setup_wineprefix.sh`. |
| `error D8050: cannot execute c1xx.dll` | Same root cause as `D8037`, one stage earlier: `cl.exe` cannot create its temp files at all. Check that the prefix is writable and complete. |
| `Wine prefix '...' is not writable` | The up-front check in `common.sh`. Point `PBS_WINEPREFIX` at a writable directory instead of waiting for the compiler's misleading error. |
| `wine: Unhandled exception 0xc0000022` noise | Harmless Wine internal message during PDB/link handling; check the exit code and the produced files. |
| `MESA: error: Failed to query drm device` | Harmless: Wine probing the GPU. The compiler does not need a display. |
| `cl.exe ... not found` | Wrong `PBS_MSVC_VER`; run `setup_wineprefix.sh` to list installed toolsets, or pass `--msvc-ver`. |
| Toolchain not found | The Windows volume is not mounted or is not auto-detected; pass `PBS_WINDOWS_ROOT=/mnt/your-volume`. |
| Sudden breakage after a Visual Studio update | Recreate the prefix: `rm -rf "$PBS_WINEPREFIX"` and re-run `setup_wineprefix.sh`; also update `PBS_MSVC_VER` if the v143 toolset moved. |
| Build is slow the first time | ~234 Wine processes are launched; subsequent builds are incremental. Raise `-j` (default: all cores). |
| `git status` warns `LF will be replaced by CRLF` | A repository file has the wrong working-copy line endings (`.gitattributes` declares `* text eol=crlf`, `*.sh text eol=lf`). Cosmetic for the build, but it turns a later normalisation into a whole-file diff. Run `scripts/linux/check_eol.py --fix`, or build with `--fix-eol`. See the section below. |
| `build_linux.sh: unexpected token $'in\r'` | The script itself was saved with CRLF endings; bash cannot match `case "$1" in`. `scripts/linux/check_eol.py --fix` restores LF (`.gitattributes` declares `*.sh text eol=lf`). |

## Line endings

`.gitattributes` declares the line endings for the repository's text files:

```
*      text eol=crlf      # source, headers, docs, vcxproj/props/sln
*.sh   text eol=lf        # shell scripts - LF only
```

Git normalises on commit, so a file with the wrong working-copy line endings still
commits cleanly and still compiles. What it breaks is the working copy: the file ends
up different from every other file, `git status` starts warning, and a later
normalisation shows up as a whole-file diff that hides the real change.

The usual cause is an editor or script that rewrites text without preserving line
endings. Python's text mode is the classic one:

```python
# wrong - rewrites every line ending to the platform default (LF on Linux)
s = open(path, encoding="utf-8").read()
open(path, "w", encoding="utf-8").write(s)

# right - read with universal newlines, write with the wanted ending
s = open(path, encoding="utf-8").read()
open(path, "w", encoding="utf-8", newline="\r\n").write(s)   # CRLF files
open(path, "w", encoding="utf-8", newline="").write(s)         # .sh files
```

The `*.sh` direction is not cosmetic: a shell script with CRLF endings makes bash fail
outright with `unexpected token $'\r'`, because `case "$1" in\r` matches nothing. After
editing any shell script, run `bash -n` on it.

```bash
python3 scripts/linux/check_eol.py          # report; exit 1 when something is wrong
python3 scripts/linux/check_eol.py --fix    # normalise, in both directions
```

`build_linux.sh` runs the check before compiling and only warns, so a stray line
ending can never fail a build. `--fix-eol` normalises first and then builds.

The checker reads `.gitattributes` for the target endings and never touches git's own
files: a `.git` directory is pruned, and the `YRpp/.git` gitlink *file* is explicitly
excluded - it contains `gitdir: ../.git/modules/YRpp`, and a trailing `\r` stops git
from resolving the submodule path.

## Testing the result

`Phobos.dll` must still be validated in game. The DLL is written to
`Release/Phobos.dll`; copy it (with `Release/Phobos.pdb` for symbolised crash
dumps) next to `gamemd.exe` in the game directory and run the game through
SyringeEx. `--stage` also copies `gamemd.edb` and, if `PBS_SYRINGE_EXE` points
at one, `Syringe.exe`.

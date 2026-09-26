#!/usr/bin/env bash
# Shared configuration and helpers for the Linux-side Phobos build.
#
# This build drives the *real* MSVC toolchain (cl.exe / rc.exe / link.exe) that
# is already installed on the Windows system, running it under Wine. Nothing is
# downloaded and no separate compiler is introduced, so the produced
# Phobos.dll is built by the exact same toolset as the Windows/CI build.
#
# Source this file; do not execute it.

# shellcheck shell=bash

set -o pipefail

pbs_info() { printf '[linux-build] %s\n' "$*" >&2; }
pbs_warn() { printf '[linux-build] warning: %s\n' "$*" >&2; }
pbs_die()  { printf '[linux-build] error: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Layout / user-tunable settings (all overridable from the environment)
# ---------------------------------------------------------------------------

PBS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PBS_ROOT="$(cd "$PBS_LINUX_DIR/../.." && pwd)"

# True when $1 is a directory this user can actually create files in. A plain
# [[ -w ]] test is not enough: the mount itself may be read-only.
pbs_dir_writable() {
  local probe
  [[ -d $1 ]] || return 1
  probe="$1/.pbs-write-probe.$$"
  if { : >"$probe"; } 2>/dev/null; then
    rm -f "$probe"
    return 0
  fi
  return 1
}

# Wine prefix. MUST live on a writable Linux-native filesystem
# (ext4/btrfs/xfs/tmpfs/...). Two different setups both surface as a compiler
# error that looks unrelated to the environment:
#   * NTFS/FUSE (ntfs-3g, exfat, ...) -> "error D8037: cannot create temporary
#     il file", because of how those filesystems handle the compiler's temp
#     files;
#   * a read-only mount -> the same D8037, or "error D8050: cannot execute
#     c1xx.dll", because cl.exe cannot create its temp files at all.
# Prefer $HOME/.cache, but fall back to a directory next to the source tree when
# $HOME is not writable (read-only root, sandbox, container), so that the
# default keeps working without any environment setup.
if [[ -z ${PBS_WINEPREFIX:-} ]]; then
  if pbs_dir_writable "$HOME/.cache"; then
    PBS_WINEPREFIX="$HOME/.cache/pbsnew-wine"
  else
    PBS_WINEPREFIX="$(cd "$PBS_ROOT/.." && pwd)/.wine-pbsnew"
  fi
fi

# MSVC toolset to use. 14.44.35207 is the last v143 (VS2022) toolset and is what
# Phobos.props' <PlatformToolset>v143</PlatformToolset> maps to on the current
# machine; override if the Windows install differs.
: "${PBS_MSVC_VER:=14.44.35207}"
: "${PBS_MSVC_ARCH:=x86}"          # target architecture
: "${PBS_SDK_VER:=}"               # empty = newest Windows 10 SDK found
: "${PBS_WINDOWS_ROOT:=}"          # empty = autodetect
: "${PBS_JOBS:=$(nproc 2>/dev/null || echo 4)}"
: "${PBS_DEBUG_INFO:=z7}"          # z7 (robust under Wine) | zi (MSBuild default)
: "${PBS_MODULES:=0}"              # 1 = also pass /experimental:module (see README)

# ---------------------------------------------------------------------------
# Locating the Windows installation
# ---------------------------------------------------------------------------

# Prints the root of the mounted Windows system volume (the one that holds
# "Program Files"), or fails.
pbs_find_windows_root() {
  local c
  if [[ -n $PBS_WINDOWS_ROOT ]]; then
    [[ -d "$PBS_WINDOWS_ROOT" ]] || pbs_die "PBS_WINDOWS_ROOT=$PBS_WINDOWS_ROOT does not exist"
    printf '%s' "$PBS_WINDOWS_ROOT"; return 0
  fi
  for c in /mnt/Windows-SSD /mnt/windows /mnt/c /mnt/C /mnt/os \
           /run/media/*/* /media/* ; do
    if [[ -d "$c/Program Files/Microsoft Visual Studio" || -d "$c/Program Files (x86)/Windows Kits" ]]; then
      printf '%s' "$c"; return 0
    fi
  done
  return 1
}

# Sets: PBS_WINDOWS_ROOT PBS_VS_ROOT PBS_MSVC_DIR PBS_VC_INCLUDE PBS_VC_LIB
#       PBS_VC_BIN PBS_VC_ATL_INCLUDE PBS_VC_ATL_LIB
pbs_locate_msvc() {
  local win vs_root kind ed
  win="$(pbs_find_windows_root)" || pbs_die \
    "could not find the Windows volume. Mount it (e.g. /mnt/Windows-SSD) or set PBS_WINDOWS_ROOT=/path"
  PBS_WINDOWS_ROOT="$win"

  for kind in "Program Files" "Program Files (x86)"; do
    for ed in Community Professional Enterprise BuildTools Preview; do
      for vs_root in "$win/$kind/Microsoft Visual Studio"/*/"$ed"; do
        [[ -d "$vs_root/VC/Tools/MSVC" ]] && { PBS_VS_ROOT="$vs_root"; break 3; }
      done
    done
  done
  [[ -n ${PBS_VS_ROOT:-} ]] || pbs_die "no Visual Studio C++ tools found under '$win/Program Files/Microsoft Visual Studio'"

  PBS_MSVC_DIR="$PBS_VS_ROOT/VC/Tools/MSVC/$PBS_MSVC_VER"
  if [[ ! -d $PBS_MSVC_DIR ]]; then
    pbs_warn "MSVC $PBS_MSVC_VER not installed; available:"
    ls -1 "$PBS_VS_ROOT/VC/Tools/MSVC" >&2 || true
    pbs_die "set PBS_MSVC_VER to one of the versions above"
  fi

  PBS_VC_INCLUDE="$PBS_MSVC_DIR/include"
  PBS_VC_LIB="$PBS_MSVC_DIR/lib/$PBS_MSVC_ARCH"
  PBS_VC_ATL_INCLUDE="$PBS_MSVC_DIR/atlmfc/include"
  PBS_VC_ATL_LIB="$PBS_MSVC_DIR/atlmfc/lib/$PBS_MSVC_ARCH"

  # Files on the Windows volume are mounted without the execute bit, so test
  # for readability only - Wine does not need +x.
  if [[ -f "$PBS_MSVC_DIR/bin/HostX64/$PBS_MSVC_ARCH/cl.exe" ]]; then
    PBS_VC_BIN="$PBS_MSVC_DIR/bin/HostX64/$PBS_MSVC_ARCH"
  elif [[ -f "$PBS_MSVC_DIR/bin/HostX86/$PBS_MSVC_ARCH/cl.exe" ]]; then
    PBS_VC_BIN="$PBS_MSVC_DIR/bin/HostX86/$PBS_MSVC_ARCH"
  else
    pbs_die "cl.exe for $PBS_MSVC_ARCH not found under $PBS_MSVC_DIR/bin"
  fi
}

# Sets: PBS_SDK_ROOT PBS_SDK_INC PBS_SDK_LIB PBS_SDK_RC
pbs_locate_sdk() {
  local kind root v versions
  for kind in "Program Files (x86)" "Program Files"; do
    for root in "$PBS_WINDOWS_ROOT/$kind/Windows Kits/10" "$PBS_WINDOWS_ROOT/Windows Kits/10"; do
      [[ -d "$root/Include" ]] && { PBS_SDK_ROOT="$root"; break 2; }
    done
  done
  [[ -n ${PBS_SDK_ROOT:-} ]] || pbs_die "Windows 10/11 SDK not found under '$PBS_WINDOWS_ROOT'"

  if [[ -n $PBS_SDK_VER ]]; then
    versions="$PBS_SDK_VER"
  else
    versions="$(ls -1 "$PBS_SDK_ROOT/Include" 2>/dev/null | sort -Vr)"
  fi
  for v in $versions; do
    [[ -d "$PBS_SDK_ROOT/Include/$v/um" && -d "$PBS_SDK_ROOT/Lib/$v/um/$PBS_MSVC_ARCH" ]] || continue
    PBS_SDK_VER="$v"; break
  done
  [[ -n ${PBS_SDK_VER:-} ]] || pbs_die "no usable Windows SDK version found under $PBS_SDK_ROOT/Include"

  PBS_SDK_INC="$PBS_SDK_ROOT/Include/$PBS_SDK_VER"
  PBS_SDK_LIB="$PBS_SDK_ROOT/Lib/$PBS_SDK_VER"
  PBS_SDK_RC="$PBS_SDK_ROOT/bin/$PBS_SDK_VER/$PBS_MSVC_ARCH/rc.exe"
  [[ -f $PBS_SDK_RC ]] || pbs_die "rc.exe not found at $PBS_SDK_RC"
}

# Everything at once, plus the include/lib directory arrays.
pbs_locate_toolchain() {
  pbs_locate_msvc
  pbs_locate_sdk

  PBS_INC_DIRS=(
    "$PBS_ROOT/src"
    "$PBS_ROOT/YRpp"
    "$PBS_VC_INCLUDE"
    "$PBS_VC_ATL_INCLUDE"
    "$PBS_SDK_INC/ucrt"
    "$PBS_SDK_INC/shared"
    "$PBS_SDK_INC/um"
    "$PBS_SDK_INC/winrt"
  )
  PBS_LIB_DIRS=(
    "$PBS_SDK_LIB/um/$PBS_MSVC_ARCH"
    "$PBS_SDK_LIB/ucrt/$PBS_MSVC_ARCH"
    "$PBS_VC_LIB"
    "$PBS_VC_ATL_LIB"
  )

  local d
  for d in "${PBS_INC_DIRS[@]}"; do
    [[ -d $d ]] || pbs_die "include directory missing: $d"
  done
  for d in "${PBS_LIB_DIRS[@]}"; do
    [[ -d $d ]] || pbs_die "library directory missing: $d"
  done
}

# ---------------------------------------------------------------------------
# Wine plumbing
# ---------------------------------------------------------------------------

# Unix path -> Wine Z: path (the whole Linux root is visible as Z:\).
pbs_w() { printf 'Z:%s' "${1//\//\\}"; }

# ';'-joined Windows list for INCLUDE / LIB / PATH style variables.
pbs_w_list() {
  local out="" d
  for d in "$@"; do
    [[ -n $out ]] && out+=";"
    out+="$(pbs_w "$d")"
  done
  printf '%s' "$out"
}

pbs_check_prefix_fs() {
  local fstype
  fstype="$(stat -f -c %T "$(dirname "$PBS_WINEPREFIX")" 2>/dev/null || true)"
  case "$fstype" in
    fuseblk|ntfs|ntfs3|vfat|msdos|exfat|nfs|cifs|smb*|9p|fuse*)
      pbs_die "PBS_WINEPREFIX is on '$fstype', which breaks MSVC under Wine (error D8037).
  Put the Wine prefix on a Linux-native filesystem, e.g.
      PBS_WINEPREFIX=\$HOME/.cache/pbsnew-wine
  Build outputs and the source tree may stay on NTFS; only the prefix (and its
  temp directory) must be native." ;;
  esac

  # The filesystem *type* can be right while the mount is still read-only (a
  # read-only bind mount, a sandboxed root, ...). That fails much later, deep
  # inside cl.exe, as a confusing D8037/D8050 - so check it up front. When the
  # prefix does not exist yet (setup) the directory that will hold it is checked.
  local target="$PBS_WINEPREFIX"
  [[ -d $target ]] || target="$(dirname "$PBS_WINEPREFIX")"
  pbs_dir_writable "$target" || pbs_die \
    "Wine prefix '$PBS_WINEPREFIX' is not writable ($target).
  cl.exe keeps its temp directory inside the prefix, so a read-only location
  makes it fail with 'error D8037' or 'error D8050'.
  Point PBS_WINEPREFIX at a writable directory on a native filesystem, e.g.
      PBS_WINEPREFIX=\$HOME/.cache/pbsnew-wine"
}

# Wine derives the Windows codepages from LC_ALL only (LANG alone is ignored).
# With a non-Chinese LC_ALL every non-ASCII character of a localized MSVC
# message is replaced by "?" before it reaches the log, which makes the Chinese
# messages of a zh-CN Visual Studio install unreadable. Point LC_ALL at a
# Chinese locale when the host has one; PBS_WINE_LOCALE overrides the choice.
pbs_resolve_wine_locale() {
  [[ ${PBS_WINE_LOCALE_SET:-0} == 1 ]] && return 0
  if [[ -z ${PBS_WINE_LOCALE:-} ]] && locale -a 2>/dev/null | grep -qi '^zh_CN\.'; then
    PBS_WINE_LOCALE=zh_CN.UTF-8
  fi
  export PBS_WINE_LOCALE="${PBS_WINE_LOCALE:-}"
  export PBS_WINE_LOCALE_SET=1
}

# Exports WINEPREFIX/WINEDEBUG/TMP/TEMP and the INCLUDE/LIB lists for the tools.
# $1 = output dir whose Generated/ header must be on the include path (optional)
pbs_wine_env() {
  local gen_dir="${1:-}"
  pbs_check_prefix_fs
  export WINEPREFIX="$PBS_WINEPREFIX"
  export WINEDEBUG="${WINEDEBUG:--all}"
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"

  # Keep wineserver's socket inside a persistent, writable directory. Wine
  # otherwise uses /tmp/.wine-<uid>; when /tmp is cleared while a wineserver is
  # still alive (sandboxes and containers often do this between sessions) the
  # next wine process cannot reach it, starts a *second* wineserver on the same
  # prefix, and the registry can end up flushed truncated - which then shows up
  # as a bogus "error D8037" on every later compile. An inherited
  # XDG_RUNTIME_DIR is only used when it is actually writable (a read-only
  # /run/user/<uid> is common in these setups).
  local pbs_rt="${XDG_RUNTIME_DIR:-}"
  if [[ -z $pbs_rt ]] || ! pbs_dir_writable "$pbs_rt"; then
    pbs_rt="$PBS_WINEPREFIX/.wine-runtime"
  fi
  if mkdir -p "$pbs_rt" 2>/dev/null; then
    chmod 700 "$pbs_rt" 2>/dev/null || true
    export XDG_RUNTIME_DIR="$pbs_rt"
  fi
  pbs_resolve_wine_locale
  [[ -n ${PBS_WINE_LOCALE:-} ]] && export LC_ALL="$PBS_WINE_LOCALE"
  # MSVC needs its temp directory inside the (native-FS) prefix. Create it only
  # when the prefix really exists: creating drive_c here would make a bare,
  # never-initialised prefix look "ready" to pbs_wine_ready.
  export TMP='C:\pbsnew-tmp'
  export TEMP='C:\pbsnew-tmp'
  if [[ -d "$PBS_WINEPREFIX/drive_c" ]]; then
    mkdir -p "$PBS_WINEPREFIX/drive_c/pbsnew-tmp"
  fi

  local inc=("${PBS_INC_DIRS[@]}")
  [[ -n $gen_dir ]] && inc=("$gen_dir" "${inc[@]}")
  export INCLUDE="$(pbs_w_list "${inc[@]}")"
  export LIB="$(pbs_w_list "${PBS_LIB_DIRS[@]}")"
}

# A prefix counts as initialised when wineboot has actually run in it. Checking
# for drive_c alone is not enough - an aborted run leaves an empty drive_c, and
# system.reg/user.reg are unreliable because Wine only flushes the registry
# files when the wineserver exits, so right after wineboot they may not exist.
pbs_wine_ready() {
  [[ -d "$PBS_WINEPREFIX/drive_c/windows" ]] || return 1
  [[ -e "$PBS_WINEPREFIX/system.reg" || -e "$PBS_WINEPREFIX/user.reg" \
     || -e "$PBS_WINEPREFIX/.update-timestamp" ]]
}

# ---------------------------------------------------------------------------
# Project file list (single source of truth: the Visual Studio project files)
# ---------------------------------------------------------------------------

# Prints one source path per line (relative to the repo root), tab separated
# from its object file path, for every ClCompile entry.
pbs_list_sources() {
  local vcxproj="$PBS_ROOT/Phobos.vcxproj"
  local yrppprops="$PBS_ROOT/YRpp/YRpp.props"
  [[ -f $vcxproj ]] || pbs_die "missing $vcxproj"

  {
    grep -o 'ClCompile Include="[^"]*"' "$vcxproj" | sed 's/^ClCompile Include="//; s/"$//'
    if [[ -f $yrppprops ]]; then
      grep -o 'ClCompile Include="[^"]*"' "$yrppprops" | sed 's/^ClCompile Include="//; s/"$//' \
        | sed 's/\$(YRppDir)/YRpp/g'
    fi
  } | sed 's/\\/\//g' | sed 's#^\./##' | awk 'NF' | sort -u
}

# Same for ResourceCompile entries.
pbs_list_resources() {
  grep -o 'ResourceCompile Include="[^"]*"' "$PBS_ROOT/Phobos.vcxproj" \
    | sed 's/^ResourceCompile Include="//; s/"$//' | sed 's/\\/\//g'
}

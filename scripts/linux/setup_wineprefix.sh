#!/usr/bin/env bash
# Creates (and validates) the Wine prefix used to run the real MSVC toolchain.
#
#   scripts/linux/setup_wineprefix.sh
#
# Environment (see common.sh): PBS_WINEPREFIX, PBS_MSVC_VER, PBS_SDK_VER,
# PBS_WINDOWS_ROOT.
#
# The Wine prefix MUST be on a Linux-native filesystem - see common.sh.

set -euo pipefail

# shellcheck source=common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

[[ $(uname -s) == Linux ]] || pbs_die "this script is for Linux"
command -v wine >/dev/null 2>&1 || pbs_die "wine is not installed"

pbs_locate_toolchain
pbs_check_prefix_fs

pbs_info "Wine prefix : $PBS_WINEPREFIX"
pbs_info "MSVC        : $PBS_MSVC_DIR"
pbs_info "Windows SDK : $PBS_SDK_VER ($PBS_SDK_ROOT)"

if pbs_wine_ready; then
  pbs_info "prefix already initialised"
else
  pbs_info "creating prefix (this takes a moment) ..."
  mkdir -p "$PBS_WINEPREFIX"
  pbs_wine_env
  if ! wineboot -u >/dev/null 2>&1; then
    pbs_warn "wineboot returned non-zero; continuing anyway"
  fi
  pbs_wine_ready || pbs_die "prefix was not created correctly at $PBS_WINEPREFIX"
fi

pbs_wine_env
mkdir -p "$PBS_WINEPREFIX/drive_c/pbsnew-tmp"

# cl.exe ships its message resources per language; 2052 = Simplified Chinese.
pbs_tool_is_localized() {
  local d
  for d in "$PBS_VC_BIN" "$PBS_VS_ROOT/Common7/IDE"; do
    [[ -d "$d/2052" ]] && return 0
  done
  return 1
}
if pbs_tool_is_localized; then
  if [[ -n ${PBS_WINE_LOCALE:-} ]]; then
    pbs_info "message locale: $PBS_WINE_LOCALE (keeps localised MSVC messages readable)"
  else
    pbs_warn "MSVC is a localised (Chinese) install but this host has no zh_CN locale,"
    pbs_warn "so its non-ASCII messages will show as '?'. Error codes, file names and"
    pbs_warn "line numbers stay readable; install a zh_CN.UTF-8 locale (or set"
    pbs_warn "PBS_WINE_LOCALE) if you want the full text."
  fi
fi

# ---------------------------------------------------------------------------
# Smoke test: compile a translation unit with the real cl.exe under Wine.
# ---------------------------------------------------------------------------
probe_dir="$PBS_WINEPREFIX/drive_c/pbsnew-tmp"
probe_log="$probe_dir/probe.log"
printf 'extern "C" int pbs_probe(void) { return 42; }\n' > "$probe_dir/probe.cpp"
rm -f "$probe_dir/probe.obj"

set +e
wine "$PBS_VC_BIN/cl.exe" /nologo /c 'C:\pbsnew-tmp\probe.cpp' \
  /Fo'C:\pbsnew-tmp\probe.obj' >"$probe_log" 2>&1
rc=$?
set -e

# cl.exe is localised (GBK/CP936 messages on a Chinese install); make the log
# readable if it is not already UTF-8.
show_log() {
  if iconv -f UTF-8 -t UTF-8 "$1" >/dev/null 2>&1; then
    grep -v 'MESA' "$1" | tail -n 15
  else
    iconv -f GBK -t UTF-8 "$1" 2>/dev/null | grep -v 'MESA' | tail -n 15 || tail -n 15 "$1"
  fi
}

if [[ $rc -ne 0 || ! -f "$probe_dir/probe.obj" ]]; then
  pbs_warn "cl.exe smoke test failed (exit $rc):"
  show_log "$probe_log" >&2
  pbs_die "the toolchain does not run under Wine; see the message above (D8037 usually means the prefix is not on a native filesystem)"
fi

pbs_info "smoke test OK - cl.exe compiled a test unit"
pbs_info "done. Now run: scripts/linux/build_linux.sh Release"

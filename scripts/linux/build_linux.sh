#!/usr/bin/env bash
# Builds Phobos.dll on Linux by running the real MSVC toolchain under Wine.
#
#   scripts/linux/build_linux.sh [Debug|Release] [options]
#
# Options:
#   --build-type NIGHTLY|RELEASE   defines NIGHTLY / RELEASE (default: none, local build)
#   -j, --jobs N                   parallel compiler invocations (default: nproc)
#   --clean                        drop all Linux-side build state and rebuild
#   --fix-eol                      normalise the line endings of repository text
#                                  files to what .gitattributes declares (CRLF),
#                                  then build. Without it the check only warns.
#   --stage                        also copy gamemd.edb (and Syringe.exe if found)
#   --deploy DIR                   copy Phobos.dll / Phobos.pdb / gamemd.edb into
#                                  DIR (the game directory). Existing files are
#                                  kept as *.prev first. PBS_DEPLOY_DIR sets the
#                                  default, so plain "build_linux.sh Release"
#                                  deploys as well.
#   -v, --verbose                  echo every compiler command; print warnings
#   --msvc-ver VER                 MSVC toolset to use (default 14.44.35207)
#   --sdk-ver VER                  Windows SDK version (default: newest installed)
#   --prefix PATH                  Wine prefix (default $HOME/.cache/pbsnew-wine,
#                                  or <repo>/../.wine-pbsnew when $HOME is not writable)
#   -h, --help
#
# The compiler/linker/resource-compiler command lines mirror what MSBuild
# produces for Phobos.vcxproj (they were taken verbatim from the
# Release/IntDir/Phobos.tlog files written by a Visual Studio build), so the
# resulting binaries are built by the same toolset with the same options as on
# Windows. See scripts/linux/README.md for the few intentional deviations.

set -euo pipefail

SELF_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
# shellcheck source=common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() { awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "$SELF_PATH"; }

cfg="Release"
build_type=""
do_clean=0
do_stage=0
do_fix_eol=0
deploy_dir=""
verbose=0
compile_one=""

while (( $# )); do
  case "$1" in
    Debug|debug)      cfg=Debug ;;
    Release|release)  cfg=Release ;;
    --build-type)     build_type="${2:?--build-type needs a value}"; shift ;;
    --build-type=*)   build_type="${1#*=}" ;;
    -j|--jobs)        PBS_JOBS="${2:?-j needs a value}"; shift ;;
    -j*)              PBS_JOBS="${1#-j}" ;;
    --clean)          do_clean=1 ;;
    --stage)          do_stage=1 ;;
    --fix-eol)        do_fix_eol=1 ;;
    --deploy)         deploy_dir="${2:?--deploy needs a directory}"; shift ;;
    --deploy=*)       deploy_dir="${1#*=}" ;;
    -v|--verbose)     verbose=1 ;;
    --msvc-ver)       PBS_MSVC_VER="${2:?}"; shift ;;
    --sdk-ver)        PBS_SDK_VER="${2:?}"; shift ;;
    --prefix)         PBS_WINEPREFIX="${2:?}"; shift ;;
    --compile-one)    compile_one="${2:?}"; shift ;;
    -h|--help)        usage; exit 0 ;;
    *)                pbs_die "unknown argument: $1 (try --help)" ;;
  esac
  shift
done

[[ $build_type == "" || $build_type == NIGHTLY || $build_type == RELEASE ]] \
  || pbs_die "--build-type must be NIGHTLY or RELEASE"
[[ $PBS_JOBS =~ ^[0-9]+$ && $PBS_JOBS -gt 0 ]] || pbs_die "-j needs a positive integer"
deploy_dir="${deploy_dir:-${PBS_DEPLOY_DIR:-}}"

# The --compile-one child re-executes this script, but its argv carries only the
# work item - the configuration has to come from the environment the parent
# exported. Without this the child would fall back to the default configuration
# and write its object files into the wrong configuration's tree.
if [[ -n $compile_one ]]; then
  cfg="${PBS_INTERNAL_CFG:?internal error: PBS_INTERNAL_CFG is not set}"
  build_type="${PBS_INTERNAL_BT:-}"
fi

command -v wine >/dev/null 2>&1 || pbs_die "wine is not installed"
pbs_locate_toolchain

# ---------------------------------------------------------------------------
# Derived paths (kept identical in the --compile-one child)
# ---------------------------------------------------------------------------
out_dir="$PBS_ROOT/$cfg"
int_dir="$out_dir/IntDir"
work_dir="$int_dir/linux"        # Linux-side build state (leaves MSBuild's own IntDir alone)
obj_dir="$work_dir/obj"
log_dir="$work_dir/log"
fail_dir="$work_dir/failed"
scratch_dir="$work_dir/scratch"
gen_dir="$int_dir/Generated"     # same location MSBuild's ComputeGitInfo target uses
dll_path="$out_dir/Phobos.dll"
pdb_path="$out_dir/Phobos.pdb"
implib_path="$int_dir/Phobos.lib"

# Export the settings the child processes need; they must resolve identically.
export PBS_WINEPREFIX PBS_MSVC_VER PBS_SDK_VER PBS_WINDOWS_ROOT PBS_JOBS
export PBS_DEBUG_INFO PBS_MODULES PBS_INTERNAL_CFG="$cfg" PBS_INTERNAL_BT="$build_type"

# ---------------------------------------------------------------------------
# Compiler flags, mirroring the CL.command.1.tlog from the Visual Studio build
# ---------------------------------------------------------------------------
pbs_cl_flags() {
  local c="$1" bt="$2"
  local -a f=(
    /c /nologo /W4 /WX- /diagnostics:column
    /DSYR_VER=2 /DHAS_EXCEPTIONS=0 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
    /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 /D_WINDLL
    '/DPHOBOS_DLL="Phobos.dll"'
    /GF /Gm- /MT /Zp8 /GS- /Gy /arch:SSE2 /fp:precise
    /Zc:wchar_t /Zc:forScope /Zc:inline /Zc:rvalueCast /GR- /openmp
    /std:c++20 /permissive- /FS /Gz /TP /external:W4 /analyze- /FC
    /wd4100 /wd4201 /wd4530 /wd4731 /wd4740 /wd4458 /wd4819 /wd5103 /wd5105
  )
  case "$c" in
    Release) f+=( /O2 /Oy ) ;;
    Debug)   f+=( /Od /Oy- /DDEBUG /wd26495 ) ;;
  esac
  case "$bt" in
    NIGHTLY) f+=( /DNIGHTLY ) ;;
    RELEASE) f+=( /DRELEASE ) ;;
  esac
  if [[ $PBS_DEBUG_INFO == zi ]]; then
    f+=( /Zi "/Fd$(pbs_w "$int_dir/vc143.pdb")" )
  else
    f+=( /Z7 )
  fi
  if [[ $PBS_MODULES == 1 ]]; then
    f+=( /experimental:module "/stdIfcDir$(pbs_w "$PBS_MSVC_DIR/ifc/$PBS_MSVC_ARCH")" )
  fi
  printf '%s\n' "${f[@]}"
}

# Makes an MSVC log readable. cl.exe/rc.exe/link.exe are localised (CP936 on a
# Chinese install) while Wine's own diagnostics are ASCII - GBK covers both, and
# "-c" drops anything that still does not convert. grep runs with -a so that a
# log containing non-UTF-8 bytes is not silently reported as "binary file
# matches" instead of being shown.
pbs_show_log() {
  local log="$1" text
  [[ -f $log ]] || return 0
  if iconv -f UTF-8 -t UTF-8 "$log" >/dev/null 2>&1; then
    text="$(cat "$log")"
  else
    # "-c" is essential: without it a single unmappable byte would make iconv
    # fail and a naive fallback would print raw GBK bytes as mojibake.
    text="$(iconv -f GBK -t UTF-8 -c "$log" 2>/dev/null || true)"
    [[ -n $text ]] || text="$(cat "$log")"
  fi
  printf '%s\n' "$text" | grep -a -v 'MESA' | tail -n 25
}

# ---------------------------------------------------------------------------
# Child mode: compile exactly one translation unit.
# ---------------------------------------------------------------------------
if [[ -n $compile_one ]]; then
  IFS=$'\t' read -r src_rel obj_rel <<<"$compile_one"
  pbs_wine_env "$gen_dir"
  mkdir -p "$obj_dir/$(dirname "$obj_rel")" "$log_dir" "$scratch_dir"
  log="$log_dir/${src_rel//\//__}.log"
  printf '  CC %s\n' "$src_rel" >&2
  set +e
  cd "$scratch_dir"
  # shellcheck disable=SC2046  # flag list is intentionally word-split
  wine "$PBS_VC_BIN/cl.exe" /nologo $(pbs_cl_flags "$PBS_INTERNAL_CFG" "$PBS_INTERNAL_BT") \
    "$(pbs_w "$PBS_ROOT/$src_rel")" "/Fo$(pbs_w "$obj_dir/$obj_rel")" >"$log" 2>&1
  rc=$?
  set -e
  if (( rc != 0 )); then
    mkdir -p "$fail_dir"
    printf '%s\n' "$src_rel" > "$fail_dir/${src_rel//\//__}"
    exit 1
  fi
  if [[ -s $log ]]; then
    filtered="$(pbs_show_log "$log")"
    if (( verbose )) || grep -qiE 'warning|error' <<<"$filtered"; then
      printf '%s\n' "$filtered" >&2
    fi
  fi
  exit 0
fi

# ---------------------------------------------------------------------------
# Main build
# ---------------------------------------------------------------------------
# Report any command that fails under "set -e" instead of exiting silently.
pbs_err_report() {
  printf '[linux-build] error: command failed (exit %d) at line %d: %s\n' "$1" "$2" "$3" >&2
}
trap 'pbs_err_report $? $LINENO "$BASH_COMMAND"' ERR

pbs_wine_env "$gen_dir"
pbs_wine_ready || pbs_die "Wine prefix '$PBS_WINEPREFIX' is not initialised - run scripts/linux/setup_wineprefix.sh first"

if (( do_clean )); then
  pbs_info "cleaning $work_dir"
  rm -rf "$work_dir"
  rm -f "$dll_path" "$pdb_path" "$implib_path"
fi
mkdir -p "$obj_dir" "$log_dir" "$scratch_dir" "$gen_dir"

# --- 0. line endings ---------------------------------------------------------
#
# .gitattributes declares `* text eol=crlf`. Git normalises on commit, so a file
# with the wrong working-copy line endings still commits cleanly and still
# compiles - the damage is invisible until someone notices a whole-file diff. The
# usual cause is editing a file with a tool that rewrites text without preserving
# line endings (Python's text-mode open() is the classic one; write with
# newline='\r\n').
#
# This is a WARNING, never an error: a stray LF does not break the build, and a
# build script must not fail for a cosmetic reason. Run --fix-eol to normalise.
if [[ -z $compile_one ]]; then
  if (( do_fix_eol )); then
    pbs_info "normalising line endings (--fix-eol)"
    python3 "$PBS_ROOT/scripts/linux/check_eol.py" --fix || true
  else
    if ! python3 "$PBS_ROOT/scripts/linux/check_eol.py"; then
      pbs_warn "line endings above differ from .gitattributes (harmless for the build);"
      pbs_warn "fix with: scripts/linux/build_linux.sh $cfg --fix-eol"
    fi
  fi
fi

# --- 1. generated version header (Phobos.props' ComputeGitInfo target) ------
pbs_generate_git_header() {
  local commit ref dirty tmp
  commit="$(git -C "$PBS_ROOT" rev-parse --short HEAD 2>/dev/null || true)"
  ref="$(git -C "$PBS_ROOT" symbolic-ref HEAD 2>/dev/null || true)"
  if [[ -z $ref && -n $commit ]]; then
    ref="$(git -C "$PBS_ROOT" for-each-ref --points-at HEAD --format='%(refname)' 2>/dev/null | sed -n '1p')"
  fi
  dirty=""
  if [[ -n $commit ]] && [[ -n $(git -C "$PBS_ROOT" status --porcelain 2>/dev/null) ]]; then
    dirty="-dirty"
  fi
  tmp="$gen_dir/Phobos.Git.h.tmp"
  {
    printf '// Generated by ComputeGitInfo - do not edit.\n'
    if [[ -n $commit ]]; then printf '#define STR_GIT_COMMIT "%s"\n' "$commit"; fi
    if [[ -n $ref ]];    then printf '#define STR_GIT_REF "%s"\n' "$ref"; fi
    if [[ -n $dirty ]];  then printf '#define STR_GIT_DIRTY "%s"\n' "$dirty"; fi
  } > "$tmp"
  if [[ -f $gen_dir/Phobos.Git.h ]] && cmp -s "$tmp" "$gen_dir/Phobos.Git.h"; then
    rm -f "$tmp"
  else
    mv "$tmp" "$gen_dir/Phobos.Git.h"
    pbs_info "generated $gen_dir/Phobos.Git.h (commit ${commit:-?}${dirty})"
  fi
}
pbs_generate_git_header

# --- 2. source list and incremental decision --------------------------------
mapfile -t sources < <(pbs_list_sources)
(( ${#sources[@]} > 0 )) || pbs_die "no sources found in Phobos.vcxproj"
mapfile -t resources < <(pbs_list_resources)

missing=0
for s in "${sources[@]}"; do
  [[ -f $PBS_ROOT/$s ]] || { pbs_warn "listed in the project but missing on disk: $s"; missing=1; }
done
(( missing == 0 )) || pbs_die "the project references files that do not exist (was a file deleted without updating Phobos.vcxproj?)"

cl_flags_str="$(pbs_cl_flags "$cfg" "$build_type" | tr '\n' ' ')"
# The toolset identity is part of the hash: switching MSVC/SDK versions must
# invalidate every object file.
flags_hash="$(printf '%s' "$cl_flags_str MSVC=$PBS_MSVC_VER SDK=$PBS_SDK_VER" | md5sum | cut -d' ' -f1)"
flags_file="$work_dir/flags.md5"
previous_hash="$(cat "$flags_file" 2>/dev/null || true)"
force_all=0
if [[ $previous_hash != "$flags_hash" ]]; then
  [[ -n $previous_hash ]] && pbs_info "compiler options changed - full rebuild"
  force_all=1
fi

# Newest header/resource in the project: any change to it invalidates every
# object (this build does not parse /showIncludes dependency files yet).
# awk (not "sort | head") is used on purpose: a truncated pipeline would make
# the shell report a spurious failure under "set -o pipefail".
stamp="$work_dir/headers.stamp"
newest_header="$(find "$PBS_ROOT/src" "$PBS_ROOT/YRpp" -type f \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.inl' -o -name '*.rc' \) \
  -printf '%T@ %p\n' 2>/dev/null | awk '
    { t = $1 + 0; sub(/^[^ ]+ /, ""); if (t > m) { m = t; newest = $0 } }
    END { if (newest != "") print newest }')"
if [[ -n $newest_header ]]; then
  touch -r "$newest_header" "$stamp"
fi
if [[ $gen_dir/Phobos.Git.h -nt $stamp ]]; then
  touch -r "$gen_dir/Phobos.Git.h" "$stamp"
fi

pending=()
for s in "${sources[@]}"; do
  obj="$obj_dir/$s.obj"
  if (( force_all )) || [[ ! -f $obj ]] || [[ ! -f $stamp ]] \
     || [[ $PBS_ROOT/$s -nt $obj ]] || [[ $stamp -nt $obj ]]; then
    pending+=("$s")
  fi
done

pbs_info "configuration : $cfg${build_type:+ / $build_type}"
pbs_info "toolchain     : MSVC $PBS_MSVC_VER + Windows SDK $PBS_SDK_VER (via $WINEPREFIX)"
pbs_info "sources       : ${#sources[@]} total, ${#pending[@]} to compile (jobs: $PBS_JOBS)"

build_start=$(date +%s)

# --- 3. compile -------------------------------------------------------------
if (( ${#pending[@]} > 0 )); then
  rm -rf "$fail_dir"
  mkdir -p "$fail_dir"
  pending_file="$work_dir/pending.txt"
  : > "$pending_file"
  for s in "${pending[@]}"; do
    printf '%s\t%s.obj\n' "$s" "$s" >> "$pending_file"
  done
  # A non-zero exit here is expected when a translation unit fails; the failure
  # markers carry the details, so keep the ERR trap out of the way.
  set +e
  trap - ERR
  xargs -d '\n' -I '{}' -P "$PBS_JOBS" bash "$SELF_PATH" --compile-one '{}' < "$pending_file"
  xargs_rc=$?
  trap 'pbs_err_report $? $LINENO "$BASH_COMMAND"' ERR
  set -e
  if (( xargs_rc != 0 )) && [[ -z "$(ls -A "$fail_dir" 2>/dev/null)" ]]; then
    pbs_die "the compiler driver failed (xargs exit $xargs_rc) - inspect $log_dir"
  fi
  failed=$(find "$fail_dir" -type f 2>/dev/null | wc -l)
  if (( failed > 0 )); then
    for f in "$fail_dir"/*; do
      rel="$(cat "$f")"
      printf '\n===== %s\n' "$rel" >&2
      pbs_show_log "$log_dir/${rel//\//__}.log" >&2
    done
    pbs_die "$failed translation unit(s) failed to compile"
  fi
  pbs_info "compiled ${#pending[@]} translation unit(s)"
fi

# --- 4. resources -----------------------------------------------------------
#
# The resources must see the same build type as the sources. src/version.rc
# includes Phobos.version.h, whose PRODUCT_VERSION / FILE_VERSION_STR differ
# between a plain local build and a NIGHTLY/RELEASE one (the latter two append
# PRERELEASE_SUFFIX). rc.exe only honours /d definitions, so the flag has to be
# repeated here - otherwise the DLL's StringFileInfo always reports a local
# build's version even when --build-type RELEASE was requested.
rc_defs=()
case "$build_type" in
  NIGHTLY) rc_defs+=( /dNIGHTLY ) ;;
  RELEASE) rc_defs+=( /dRELEASE ) ;;
esac

link_input=()
link_input_win=()
for r in "${resources[@]}"; do
  res="$int_dir/$(basename "${r%.rc}").res"
  # A .res bakes in the preprocessor state of the .rc it came from. Comparing it
  # against the .rc timestamp alone is not enough: switching --build-type does not
  # touch the .rc's timestamp but does change the version string inside, so a local
  # build followed by a RELEASE build would silently ship the local build's
  # FileVersion/ProductVersion metadata. force_all is set whenever the
  # compiler-option hash (which includes /DRELEASE or /DNIGHTLY) changes, so gate
  # the resources on it too. The .res lives in MSBuild's own IntDir, which this
  # script otherwise leaves alone, hence the explicit invalidation here.
  if (( force_all )) || [[ ! -f $res || $PBS_ROOT/$r -nt $res ]]; then
    pbs_info "RC ${r}"
    ( cd "$scratch_dir" && wine "$PBS_SDK_RC" /l0x0409 /nologo "${rc_defs[@]+"${rc_defs[@]}"}" \
        "/fo$(pbs_w "$res")" "$(pbs_w "$PBS_ROOT/$r")" ) >"$log_dir/rc_$(basename "$r").log" 2>&1 \
      || { pbs_show_log "$log_dir/rc_$(basename "$r").log" >&2; pbs_die "resource compilation failed for $r"; }
  fi
  link_input+=("$res"); link_input_win+=("$(pbs_w "$res")")
done

for s in "${sources[@]}"; do
  link_input+=("$obj_dir/$s.obj"); link_input_win+=("$(pbs_w "$obj_dir/$s.obj")")
done

# --- 5. link ----------------------------------------------------------------
needs_link=0
if [[ ! -f $dll_path ]]; then
  needs_link=1
elif (( ${#pending[@]} > 0 )); then
  needs_link=1
else
  for f in "${link_input[@]}"; do
    [[ $f -nt $dll_path ]] && { needs_link=1; break; }
  done
fi

if (( needs_link )); then
  pbs_info "linking $dll_path"
  # Clear previous outputs first: link.exe refuses to overwrite a truncated or
  # corrupt import library (LNK1136) left behind by an interrupted build.
  rm -f "$dll_path" "$pdb_path" "$implib_path" "${implib_path%.lib}.exp"
  # Scratch is the working directory of the tools (rc.exe and link.exe drop
  # temporary/default-named files there).
  find "$scratch_dir" -mindepth 1 -delete 2>/dev/null || true
  link_args=(
    /NOLOGO /DLL /MACHINE:X86 /INCREMENTAL:NO
    "/OUT:$(pbs_w "$dll_path")" "/PDB:$(pbs_w "$pdb_path")" "/IMPLIB:$(pbs_w "$implib_path")"
    /DEBUG:FULL /MANIFEST:NO /SUBSYSTEM:WINDOWS
    /OPT:REF /OPT:ICF /TLBID:1 /DYNAMICBASE /NXCOMPAT /SAFESEH
    dbghelp.lib onecore.lib
  )
  link_log="$log_dir/link.log"
  set +e
  ( cd "$scratch_dir" && wine "$PBS_VC_BIN/link.exe" "${link_args[@]}" "${link_input_win[@]}" ) \
    >"$link_log" 2>&1
  link_rc=$?
  set -e
  if (( link_rc != 0 )) || [[ ! -f $dll_path ]]; then
    pbs_show_log "$link_log" >&2
    pbs_die "linking failed (exit $link_rc, see $link_log)"
  fi
  if grep -qiE 'warning|error' "$link_log"; then pbs_show_log "$link_log" >&2; fi
else
  pbs_info "up to date - nothing to link"
fi

# --- 6. staging (optional) --------------------------------------------------
if (( do_stage )); then
  [[ -f $PBS_ROOT/gamemd.edb ]] && cp -f "$PBS_ROOT/gamemd.edb" "$out_dir/gamemd.edb" \
    && pbs_info "staged gamemd.edb"
  syringe="${PBS_SYRINGE_EXE:-}"
  if [[ -n $syringe && -f $syringe ]]; then
    cp -f "$syringe" "$out_dir/Syringe.exe" && pbs_info "staged Syringe.exe"
  else
    pbs_warn "Syringe.exe not staged (set PBS_SYRINGE_EXE=/path/to/Syringe.exe to bundle it)"
  fi
fi

# --- 7. deploy (optional) ---------------------------------------------------
if [[ -n $deploy_dir ]]; then
  [[ -d $deploy_dir ]] || pbs_die "--deploy: '$deploy_dir' is not a directory"
  [[ -f $deploy_dir/gamemd.exe ]] || pbs_warn "'$deploy_dir' contains no gamemd.exe - is that really the game directory?"

  deploy_one() {
    local name="$1" src="$2"
    [[ -f $src ]] || { pbs_warn "cannot deploy $name: $src does not exist"; return 0; }
    if [[ -f $deploy_dir/$name ]] && cmp -s "$src" "$deploy_dir/$name"; then
      pbs_info "$name unchanged in $deploy_dir"
      return 0
    fi
    if [[ -f $deploy_dir/$name ]]; then
      cp -f "$deploy_dir/$name" "$deploy_dir/$name.prev"
      pbs_info "kept the previous $name as $name.prev"
    fi
    cp -f "$src" "$deploy_dir/$name"
    pbs_info "deployed $name -> $deploy_dir"
  }
  deploy_one Phobos.dll "$dll_path"
  deploy_one Phobos.pdb "$pdb_path"
  deploy_one gamemd.edb "$PBS_ROOT/gamemd.edb"
fi

printf '%s' "$flags_hash" > "$flags_file"

elapsed=$(( $(date +%s) - build_start ))
printf '\n'
pbs_info "done in ${elapsed}s"
ls -l "$dll_path" "$pdb_path" 2>/dev/null >&2 || true
pbs_info "output: $dll_path"

#!/bin/bash
# End-to-end zstd tests against a built AFNI.  usage: e2e.sh <afni src dir> <input.nii.gz> <workdir>
set -u
A=$1; IN=$2; W=$3
mkdir -p "$W"; cd "$W" || exit 1
export PATH="$A:$PATH" AFNI_NIFTI_TYPE_WARN=NO AFNI_NOSPLASH=YES
fails=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; fails=$((fails+1)); }
t()    { local s=$(date +%s.%N); "$@" > /dev/null 2>&1; local rc=$?; printf "%6.2fs  %s\n" "$(echo "$(date +%s.%N) - $s" | bc)" "$*"; return $rc; }
fsize()  { if stat -c %s "$1" > /dev/null 2>&1; then stat -c %s "$1"; else stat -f %z "$1"; fi; }   # GNU / BSD
maxdiff() { rm -f diff_tmp+orig.*; 3dcalc -overwrite -a "$1" -b "$2" -expr 'abs(a-b)' -prefix diff_tmp+orig > /dev/null 2>&1 &&
            3dBrickStat -slow -max diff_tmp+orig.HEAD 2>/dev/null | tr -d ' '; rm -f diff_tmp+orig.*; }

echo "== which: $(which 3dcopy)"
echo "== input: $IN ($(fsize "$IN") bytes)"
sync; cat "$IN" > /dev/null

echo; echo "== writes"
t 3dcopy -overwrite "$IN" ref.nii          || fail "3dcopy -> .nii"
t 3dcopy -overwrite ref.nii out.nii.gz     || fail "3dcopy -> .nii.gz"
t 3dcopy -overwrite ref.nii out.nii.zst    || fail "3dcopy -> .nii.zst"
AFNI_COMPRESSOR=GZIP t 3dcopy -overwrite ref.nii out_gz+orig  || fail "3dcopy -> BRIK.gz"
AFNI_COMPRESSOR=ZSTD t 3dcopy -overwrite ref.nii out_zst+orig || fail "3dcopy -> BRIK.zst"
ls -l ref.nii out.nii.gz out.nii.zst out_gz+orig.BRIK* out_zst+orig.BRIK* 2>/dev/null | awk '{printf "  %12d  %s\n", $5, $9}'
[ -f out_zst+orig.BRIK.zst ] && pass "BRIK written as .BRIK.zst" || fail "no .BRIK.zst written"
head -c 4 out.nii.zst | xxd -p | grep -q '^502a4d18' && pass ".nii.zst starts with pzstd frame index" || fail ".nii.zst frame index"

echo; echo "== data identical to uncompressed"
for f in out.nii.gz out.nii.zst out_zst+orig.HEAD; do
  d=$(maxdiff ref.nii $f); [ "$d" = "0" ] && pass "$f max|diff| = 0" || fail "$f max|diff| = $d"
done
zstd -dqc out.nii.zst > ext_zstd.nii  2>/dev/null
pzstd -dqc out.nii.zst > ext_pzstd.nii 2>/dev/null
cmp -s ext_zstd.nii ext_pzstd.nii && pass "zstd -d and pzstd -d agree" || fail "zstd -d and pzstd -d differ"
for f in ext_zstd.nii ext_pzstd.nii; do
  d=$(maxdiff ref.nii $f); [ "$d" = "0" ] && pass "$f (external decompress) max|diff| = 0" || fail "$f max|diff| = $d"
done
rm -f ext_zstd.nii ext_pzstd.nii

echo; echo "== foreign zstd files (CLI single frame, pzstd)"
zstd -qf -T0 ref.nii -o cli.nii.zst; pzstd -qf ref.nii -o pz.nii.zst
for f in cli.nii.zst pz.nii.zst; do
  d=$(maxdiff ref.nii $f); [ "$d" = "0" ] && pass "$f readable, max|diff| = 0" || fail "$f max|diff| = $d"
done

echo; echo "== header-only reads (3dinfo): bytes read from file"
have_strace=1; command -v strace > /dev/null 2>&1 || have_strace=0
[ $have_strace -eq 0 ] && echo "  (no strace: skipping byte counts, timings below still apply)"
for f in out.nii.gz out.nii.zst cli.nii.zst; do
  [ $have_strace -eq 0 ] && continue
  rb=$(strace -f -e trace=openat,read,pread64 -e signal=none 3dinfo -nv $f 2>&1 >/dev/null |
       awk -v f="$f" '/openat/ && index($0,f) { match($0,/= [0-9]+$/); fd=substr($0,RSTART+2) }
                      fd!="" && /^(\[pid +[0-9]+\] )?(read|pread64)\(/ { split($0,a,"("); split(a[2],b,","); if (b[1]==fd) { match($0,/= [0-9]+$/); n+=substr($0,RSTART+2) } }
                      END { print n+0 }')
  printf "  %-14s %10d bytes read of %d\n" $f "$rb" "$(fsize $f)"
done
t 3dinfo out.nii.gz; t 3dinfo out.nii.zst; t 3dinfo out_zst+orig.HEAD

echo; echo "== 3drefit"
cp out.nii.zst refit.nii.zst
t 3drefit -TR 1.234 refit.nii.zst || fail "3drefit .nii.zst"
[ "$(3dinfo -tr refit.nii.zst 2>/dev/null)" = "1.234000" ] && pass "3drefit TR stuck on .nii.zst" || fail "3drefit TR: $(3dinfo -tr refit.nii.zst 2>&1)"
d=$(maxdiff ref.nii refit.nii.zst); [ "$d" = "0" ] && pass "3drefit kept data" || fail "3drefit data max|diff| = $d"

echo; echo "== sibling files: x.nii.gz and x.nii.zst with different data"
3dcalc -overwrite -a ref.nii -expr 'a+1' -prefix sib.nii.gz > /dev/null 2>&1
cp out.nii.zst sib.nii.zst
d=$(maxdiff ref.nii sib.nii.zst); [ "$d" = "0" ] && pass "sib.nii.zst read its own data" || fail "sib.nii.zst read sibling (max|diff| = $d)"
d=$(maxdiff ref.nii sib.nii.gz);  [ "$d" = "1" ] && pass "sib.nii.gz read its own data"  || fail "sib.nii.gz max|diff| = $d (expect 1)"

echo; echo "== read speed (3dTstat -mean)"
for f in ref.nii out.nii.gz out.nii.zst pz.nii.zst cli.nii.zst out_gz+orig.HEAD out_zst+orig.HEAD; do
  t 3dTstat -overwrite -mean -prefix mean.nii $f
done

echo; echo "== compression levels (native .nii.zst)"
for L in 1 3 6; do
  AFNI_ZSTD_LEVEL=$L t 3dcopy -overwrite ref.nii lev$L.nii.zst
  printf "  level %d: %d bytes\n" $L $(fsize lev$L.nii.zst)
done

echo; [ $fails -eq 0 ] && echo "ALL PASSED" || echo "$fails FAILURES"

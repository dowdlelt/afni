#!/bin/bash
# Try to reproduce the AFNI GUI graph problem with .nii.zst datasets.
# usage: gui.sh <afni bin dir> <source dataset> <workdir> [suffix]
set -u
A=$1; SRC=$2; W=$3; SUF=${4:-.nii.zst}
export PATH="$A:$PATH" AFNI_NOSPLASH=YES AFNI_SPLASH_MESSAGE=NO AFNI_DONT_LOGFILE=YES
rm -rf "$W"; mkdir -p "$W"; cd "$W" || exit 1

# two small 4D datasets in the same session
3dTcat -overwrite -prefix one$SUF "$SRC[0..29]"  > /dev/null 2>&1
3dTcat -overwrite -prefix two$SUF "$SRC[30..59]" > /dev/null 2>&1
ls -l one* two* | awk '{printf "  %12d  %s\n", $5, $9}'

run() {
  local name=$1; shift
  echo "-- $name"
  timeout 180 xvfb-run -a "$@" > $name.out 2>&1
  local rc=$?
  echo "   exit=$rc"
  [ $rc -ge 128 ] && echo "   *** signal $((rc-128)) (139 = segfault)"
  grep -iE "fatal|segmentation|cannot open|can't open|failed|\*\* ERROR" $name.out | sort -u | head -5
  return $rc
}

# 1. open a graph on the first dataset, then switch underlay to the second
run graph_switch afni -com 'OPEN_WINDOW A.axialimage' \
                      -com 'OPEN_WINDOW A.axialgraph' \
                      -com 'SET_SUBBRICK A.0' \
                      -com 'SWITCH_UNDERLAY two' \
                      -com 'OPEN_WINDOW A.axialgraph' \
                      -com 'QUIT' .

# 2. same, but load the second dataset as the overlay
run graph_overlay afni -com 'OPEN_WINDOW A.axialgraph' \
                       -com 'SWITCH_OVERLAY two' \
                       -com 'QUIT' .

# 3. two controllers, one dataset each, both graphing
run two_controllers afni -com 'OPEN_WINDOW A.axialgraph' \
                         -com 'NEW_CONTROLLER B' \
                         -com 'SWITCH_UNDERLAY B.two' \
                         -com 'OPEN_WINDOW B.axialgraph' \
                         -com 'QUIT' .

echo "-- logs in $W"

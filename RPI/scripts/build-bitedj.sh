#!/bin/bash
# Driver for the BiteDJ build on the Pi. Retries on OOM kills (ccache + make resume,
# so nothing is lost), drops to -j1 when -j2 keeps dying on the same TU.
cd "$HOME/bitedj" || exit 1
LOG="$HOME/bitedj-build.log"
for attempt in 1 2 3 4 5 6 7 8; do
  if [ "$attempt" -ge 4 ]; then J=1; else J=2; fi
  echo "=== ATTEMPT $attempt  -j$J  $(date -Is) ===" | tee -a "$LOG"
  if cmake --build build -j$J 2>&1 | tee -a "$LOG"; then
    echo "=== BUILD_SUCCESS $(date -Is) ===" | tee -a "$LOG"
    exit 0
  fi
  # Only keep going if it looks like the OOM killer, not a real compile error.
  if ! tail -80 "$LOG" | grep -qiE "signal 9|Killed|out of memory|cc1plus.*terminated"; then
    echo "=== BUILD_FAILED (real error, not OOM) $(date -Is) ===" | tee -a "$LOG"
    exit 1
  fi
  echo "=== OOM kill detected, retrying ===" | tee -a "$LOG"
done
echo "=== BUILD_FAILED (attempts exhausted) $(date -Is) ===" | tee -a "$LOG"
exit 1

#!/usr/bin/env bash
# Emulator tests against a REAL card: emu-tests-real/*.emu with --card pointed
# at your own breaks. The card is never committed (copyrighted recordings), so
# with no card this says so and exits 0.
#
#   tools/test-real-card.sh                       # card at ~/Music/Break Card
#   BREAK_REAL_CARD=/Volumes/CARD tools/test-real-card.sh
#
# Needs the alchemy-lab emulator (branch emu-custom). EMU_DIR points at its
# emu/ folder; EMU_ALCHEMY_DIR / EMU_LIBDAISY_DIR as in emu.mk when this
# checkout's lib/ submodules are empty.
set -u
cd "$(dirname "$0")/.."
FW_DIR=$PWD
CARD=${BREAK_REAL_CARD:-$HOME/Music/Break Card}
EMU_DIR=${EMU_DIR:-$HOME/tim-os/alchemy-lab/.claude/worktrees/emu/emu}

if [ ! -d "$CARD/bank1" ]; then
  echo "skip: no real card at $CARD (build one: python3 tools/make_card.py <breaks> \"$CARD\")"
  exit 0
fi
if [ -z "${EMU_ALCHEMY_DIR:-}" ] && [ ! -d lib/alchemy-sdk/framework ]; then
  main=$(git worktree list --porcelain | awk '/^worktree /{print $2; exit}')
  export EMU_ALCHEMY_DIR=$main/lib/alchemy-sdk EMU_LIBDAISY_DIR=$main/lib/libDaisy
fi

make -s -C "$EMU_DIR" FW=custom FW_DIR="$FW_DIR" -j8 >/dev/null || { echo "emulator build failed"; exit 1; }
EMU=$(ls -td "$EMU_DIR"/build/custom/break_alchemy-*/emu | head -1)

mkdir -p build/emu-real
fail=0
for t in emu-tests-real/*.emu; do
  log=build/emu-real/$(basename "$t" .emu).txt
  ( "$EMU" --headless --flash none --card "$CARD" --script "$t" > "$log" 2>&1 ) &
  pid=$! waited=0
  while kill -0 $pid 2>/dev/null; do
    sleep 1; waited=$((waited + 1))
    if [ $waited -ge 180 ]; then kill -9 $pid 2>/dev/null; echo "HANG" >> "$log"; break; fi
  done
  wait $pid; st=$?
  p=$(grep -c '^PASS' "$log"); x=$(grep -c '^FAIL' "$log")
  if [ "$x" -eq 0 ] && [ "$st" -eq 0 ]; then m=ok; else m=FAIL; fail=1; fi
  printf '%-4s %-10s %3d pass %2d fail   (%s)\n' "$m" "$(basename "$t" .emu)" "$p" "$x" "$log"
  grep -E '^FAIL|HANG' "$log" | sed 's/^/       /'
done
exit $fail

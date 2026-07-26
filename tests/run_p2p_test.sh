#!/usr/bin/env bash
# Launches two test_netplay_p2p processes against each other on loopback
# and asserts their final confirmed hashes match. Usage: run_p2p_test.sh <binary>
set -u
BIN="$1"
TICKS=600

# Scratch dir for the two processes' output. `mktemp -d` alone is not enough:
# under MSYS2 bash it can hand back a path it cannot then write into (its /tmp
# maps to C:/msys64/tmp and the create/write disagree), which failed this test
# for environment reasons that have nothing to do with the netcode. So pick the
# first candidate we can actually write a file into, falling back to a dir
# beside the test binary — always writable, since we just built there.
DIR=""
for cand in "$(mktemp -d 2>/dev/null || true)" \
            "${TMPDIR:-}/p2p_$$" "$(dirname "$BIN")/p2p_scratch_$$"; do
    [ -n "$cand" ] || continue
    mkdir -p "$cand" 2>/dev/null || continue
    if : > "$cand/.wtest" 2>/dev/null; then rm -f "$cand/.wtest"; DIR="$cand"; break; fi
done
[ -n "$DIR" ] || { echo "netplay_p2p: no writable scratch dir"; exit 1; }
trap 'rm -rf "$DIR"' EXIT

"$BIN" --port 7101 --peer 127.0.0.1:7102 --player 0 --ticks $TICKS > "$DIR/a.txt" 2>&1 &
PA=$!
"$BIN" --port 7102 --peer 127.0.0.1:7101 --player 1 --ticks $TICKS > "$DIR/b.txt" 2>&1
RB=$?
wait $PA
RA=$?

echo "--- player 0:"; cat "$DIR/a.txt"
echo "--- player 1:"; cat "$DIR/b.txt"
[ $RA -eq 0 ] && [ $RB -eq 0 ] || { echo "netplay_p2p: process failure"; exit 1; }

A=$(grep '^p2p ' "$DIR/a.txt")
B=$(grep '^p2p ' "$DIR/b.txt")
if [ -n "$A" ] && [ "$A" = "$B" ]; then
    echo "netplay_p2p: MATCH - $A"
else
    echo "netplay_p2p: HASH MISMATCH"
    exit 1
fi

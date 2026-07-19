#!/usr/bin/env bash
# Launches two test_netplay_p2p processes against each other on loopback
# and asserts their final confirmed hashes match. Usage: run_p2p_test.sh <binary>
set -u
BIN="$1"
TICKS=600
DIR="$(mktemp -d)"
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

#!/usr/bin/env bash
# run_mesh_test.sh <mesh_bin> <rendezvous_bin> <variant>
# variant: direct | relay | impair
#
# Boots a real arena_rendezvous, one --host and three --join processes against
# it, and asserts all four confirmed hashes are identical. The rendezvous log is
# the evidence for HOW the traffic travelled: 'relay active' must appear in the
# relay variant and must NOT appear in the direct one.
set -u
BIN="$1"; RV="$2"; VARIANT="${3:-direct}"
TICKS=600
PORT=$((47000 + RANDOM % 2000))

# Scratch dir for the processes' output. `mktemp -d` alone is not enough:
# under MSYS2 bash it can hand back a path it cannot then write into (its /tmp
# maps to C:/msys64/tmp and the create/write disagree), which failed this test
# for environment reasons that have nothing to do with the netcode. So pick the
# first candidate we can actually write a file into, falling back to a dir
# beside the test binary — always writable, since we just built there.
DIR=""
for cand in "$(mktemp -d 2>/dev/null || true)" \
            "${TMPDIR:-}/mesh_$$" "$(dirname "$BIN")/mesh_scratch_$$"; do
    [ -n "$cand" ] || continue
    mkdir -p "$cand" 2>/dev/null || continue
    if : > "$cand/.wtest" 2>/dev/null; then rm -f "$cand/.wtest"; DIR="$cand"; break; fi
done
[ -n "$DIR" ] || { echo "mesh: no writable scratch dir"; exit 1; }

EXTRA=""
case "$VARIANT" in
  relay)  EXTRA="--forced-relay" ;;
  impair) EXTRA="--impair wan100" ;;
esac

"$RV" --port $PORT > "$DIR/rv.txt" 2>&1 &
RVPID=$!
trap 'kill $RVPID 2>/dev/null; rm -rf "$DIR"' EXIT
sleep 1

"$BIN" --server 127.0.0.1:$PORT --host 4 --ticks $TICKS $EXTRA > "$DIR/p0.txt" 2>&1 &
P0=$!
CODE=""
for i in $(seq 1 50); do
    CODE=$(grep -m1 '^code ' "$DIR/p0.txt" 2>/dev/null | cut -d' ' -f2)
    [ -n "$CODE" ] && break
    sleep 0.2
done
[ -n "$CODE" ] || { echo "mesh: no lobby code from host"; cat "$DIR/p0.txt"; exit 1; }

for i in 1 2 3; do
    "$BIN" --server 127.0.0.1:$PORT --join "$CODE" --ticks $TICKS $EXTRA > "$DIR/p$i.txt" 2>&1 &
    eval "P$i=\$!"
done
FAIL=0
for i in 0 1 2 3; do
    eval "wait \$P$i"; RC=$?
    [ $RC -eq 0 ] || { echo "mesh: player $i exit $RC"; FAIL=1; }
done
for i in 0 1 2 3; do echo "--- player $i:"; cat "$DIR/p$i.txt"; done
echo "--- rendezvous:"; cat "$DIR/rv.txt"
[ $FAIL -eq 0 ] || exit 1

REF=$(grep '^mesh ' "$DIR/p0.txt" | sed 's/slot=[0-9]//')
for i in 1 2 3; do
    CUR=$(grep '^mesh ' "$DIR/p$i.txt" | sed 's/slot=[0-9]//')
    [ "$CUR" = "$REF" ] || { echo "mesh: HASH MISMATCH p$i"; exit 1; }
done
# How the traffic travelled, asserted from the server's own log. The negative
# case matters as much as the positive one: without it, a direct run in which
# punching silently broke and everything fell back to relay would still pass.
if [ "$VARIANT" = "relay" ]; then
    grep -q '^relay active' "$DIR/rv.txt" || { echo "mesh: relay carried no packets"; exit 1; }
else
    ! grep -q '^relay active' "$DIR/rv.txt" || { echo "mesh: traffic fell back to RELAY"; exit 1; }
fi
echo "mesh($VARIANT): MATCH - $REF"
exit 0

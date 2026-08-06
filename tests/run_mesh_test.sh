#!/usr/bin/env bash
# run_mesh_test.sh <mesh_bin> <rendezvous_bin> <variant> [replay_bundle_bin]
# variant: direct | relay | impair | inject
#
# Boots a real arena_rendezvous, one --host and three --join processes against
# it, and asserts all four confirmed hashes are identical. The rendezvous log is
# the evidence for HOW the traffic travelled: 'relay active' must appear in the
# relay variant and must NOT appear in the direct one.
#
# The inject variant asserts the opposite outcome: joiner 1 corrupts its own sim
# at tick 300, so the detector MUST fire, bundles MUST be written, and
# replay_bundle MUST localise the corrupted tick and name slot 1 as the culprit.
# It is the falsifiability proof for the desync pipeline - without it, none of
# that machinery has ever been observed to work.
set -u
BIN="$1"; RV="$2"; VARIANT="${3:-direct}"; REPLAY="${4:-}"
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
  inject) EXTRA="--bundle-dir $DIR"     # --inject goes ONLY to joiner 1, below
          [ -n "$REPLAY" ] || { echo "mesh(inject): needs the replay_bundle path as argv 4"; exit 1; } ;;
esac
INJECT_AT=300

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
    ONE=""
    [ "$VARIANT" = "inject" ] && [ $i -eq 1 ] && ONE="--inject $INJECT_AT"
    "$BIN" --server 127.0.0.1:$PORT --join "$CODE" --ticks $TICKS $EXTRA $ONE > "$DIR/p$i.txt" 2>&1 &
    eval "P$i=\$!"
done
FAIL=0
for i in 0 1 2 3; do
    eval "wait \$P$i"; eval "RC$i=\$?"
    eval "RC=\$RC$i"
    # the inject variant EXPECTS nonzero exits; it checks them itself below
    if [ "$VARIANT" != "inject" ] && [ $RC -ne 0 ]; then
        echo "mesh: player $i exit $RC"; FAIL=1
    fi
done
for i in 0 1 2 3; do echo "--- player $i:"; cat "$DIR/p$i.txt"; done
echo "--- rendezvous:"; cat "$DIR/rv.txt"
[ $FAIL -eq 0 ] || exit 1

if [ "$VARIANT" = "inject" ]; then
    # PROCESS INDEX IS NOT SLOT INDEX. A slot is assigned by JOIN-ARRIVAL order at
    # the rendezvous, not by the order we launch the joiners - and on a fast host
    # (Linux CI) the three joiners genuinely race, so the process we handed
    # --inject to can land in any slot. Every bundle filename is slot-indexed, so
    # the assertions below read each process's own `routes slot=` line and never
    # assume p<i> == slot <i>. Getting this wrong made CI red while Windows, where
    # the joiners happened not to race, stayed green.
    slot_of() {   # slot index from a client's own "routes slot=N ..." line
        grep -m1 '^routes slot=' "$1" | tr ' ' '\n' | grep -m1 '^slot=' \
            | cut -d= -f2 | tr -d '\r'
    }
    # 1. the injected client detected its own corruption and exited 3
    #    (process-indexed, and correctly so: p1 is the one we injected)
    [ "$RC1" -eq 3 ] || { echo "mesh(inject): p1 exit $RC1, expected 3"; exit 1; }
    grep -q '^desync tick=' "$DIR/p1.txt" || { echo "mesh(inject): p1 printed no desync line"; exit 1; }
    CULPRIT_SLOT=$(slot_of "$DIR/p1.txt")
    [ -n "$CULPRIT_SLOT" ] || { echo "mesh(inject): p1 never printed a routes slot= line"; exit 1; }
    echo "mesh(inject): injected process p1 was assigned slot $CULPRIT_SLOT"
    # 2. a checksum disagreement has two sides: some OTHER client must see it too
    OTHER=""; WITNESS_SLOT=""
    for i in 0 2 3; do
        eval "RC=\$RC$i"
        [ "$RC" -eq 3 ] || continue
        S=$(slot_of "$DIR/p$i.txt")
        [ -n "$S" ] && [ "$S" != "$CULPRIT_SLOT" ] || continue
        OTHER=$i; WITNESS_SLOT=$S; break
    done
    [ -n "$OTHER" ] || { echo "mesh(inject): no other client witnessed the desync (exits: $RC0 $RC1 $RC2 $RC3)"; exit 1; }
    echo "mesh(inject): p1 (slot $CULPRIT_SLOT) and p$OTHER (slot $WITNESS_SLOT) both exited 3"
    CULPRIT_BIN="$DIR/desync_slot$CULPRIT_SLOT.bin"
    WITNESS_BIN="$DIR/desync_slot$WITNESS_SLOT.bin"
    # 3. bundles on disk: the culprit's, plus the witness's
    [ -f "$CULPRIT_BIN" ] || { echo "mesh(inject): no $CULPRIT_BIN"; exit 1; }
    [ -f "$WITNESS_BIN" ] || { echo "mesh(inject): no $WITNESS_BIN"; exit 1; }
    # 4. offline localisation: the corrupted tick, not the detection tick
    echo "--- replay_bundle (single):"
    "$REPLAY" "$CULPRIT_BIN" > "$DIR/rb1.txt" 2>&1 || {
        echo "mesh(inject): replay_bundle failed"; cat "$DIR/rb1.txt"; exit 1; }
    cat "$DIR/rb1.txt"
    # Field-sliced, NOT `sed -n 's/.*tick=\(...\)/\1/p'`: the bash CMake finds for
    # ctest here (C:/msys64/usr/bin/bash.exe) ships a sed whose BRE capture groups
    # do not work - `echo hello123world | sed -n 's/.*hello\([0-9]*\).*/\1/p'`
    # prints nothing, while `sed -E` and plain field slicing both behave. Cost a
    # green-outside-ctest / red-inside-ctest debugging round; do not reintroduce.
    N=$(grep -m1 '^DIVERGED ' "$DIR/rb1.txt" | tr ' ' '\n' | grep -m1 '^tick=' \
        | cut -d= -f2 | tr -d '\r')
    [ -n "$N" ] || { echo "mesh(inject): replay_bundle found no DIVERGED tick"; exit 1; }
    [ "$N" -ge $INJECT_AT ] && [ "$N" -le $((INJECT_AT + 20)) ] || {
        echo "mesh(inject): divergence tick $N outside [$INJECT_AT,$((INJECT_AT + 20))]"; exit 1; }
    echo "mesh(inject): localised divergence at tick $N (injected at $INJECT_AT)"
    # 5. two-bundle mode names the culprit and clears the witness
    echo "--- replay_bundle (pair):"
    "$REPLAY" "$CULPRIT_BIN" "$WITNESS_BIN" > "$DIR/rb2.txt" 2>&1 || {
        echo "mesh(inject): replay_bundle pair mode failed"; cat "$DIR/rb2.txt"; exit 1; }
    cat "$DIR/rb2.txt"
    grep -q "^DIVERGED bundle=.*desync_slot$CULPRIT_SLOT.bin" "$DIR/rb2.txt" || {
        echo "mesh(inject): slot $CULPRIT_SLOT not reported DIVERGED in pair mode"; exit 1; }
    grep -q "^CONSISTENT bundle=.*desync_slot$WITNESS_SLOT.bin" "$DIR/rb2.txt" || {
        echo "mesh(inject): slot $WITNESS_SLOT not reported CONSISTENT in pair mode"; exit 1; }
    grep -q "^CULPRIT .*desync_slot$CULPRIT_SLOT.bin" "$DIR/rb2.txt" || {
        echo "mesh(inject): culprit not named as slot $CULPRIT_SLOT"; exit 1; }
    echo "mesh(inject): PASS - detector fired, bundles written, tick $N localised, slot $CULPRIT_SLOT named"
    exit 0
fi

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

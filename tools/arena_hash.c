/* Scripted-match hash generator — the cross-platform determinism pin.
 * The match itself lives in src/arena/arena_script.h (shared with the
 * netplay version handshake). Any behavior change breaks the pinned hash:
 * the gate proves this file and the header still agree with the pin. */
#include <stdio.h>
#include "../src/arena/arena_script.h"

int main(void) {
    printf("%08x\n", arena_scripted_match_hash());
    return 0;
}

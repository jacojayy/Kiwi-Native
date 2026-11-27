// crypto_test — nx_crypto.c's AES: known answers, and the hardware cipher (AES-NI, ARMv8) against the portable one (tests/nx_selftest.h has the list).
// No keys and no game needed.
//   crypto_test [rounds = 300]      rounds: how many random CTR cases of up to 4 MB
// Exit status 0 when every check passed. Worth running once under -fsanitize=address,undefined: every buffer is allocated at its exact size.
#include "nx_selftest.h"

int main(int argc, char **argv) {
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 300;
    int bad = nx_selftest_crypto(rounds);
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

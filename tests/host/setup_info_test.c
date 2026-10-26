// Cross-checks tools/homekit_provision.py against the firmware crypto backend:
// the SRP verifier computed in Python must match what nrf_oberon derives from
// the same salt and setup code.

#include "HAPPlatform.h"
#include "HAPCrypto.h"

#include <stdio.h>
#include <string.h>

#include "homekit_setup_info.h"

#ifndef HOMEKIT_TEST_SETUP_CODE
#error "HOMEKIT_TEST_SETUP_CODE must be defined"
#endif

int main(void) {
    static const uint8_t salt[SRP_SALT_BYTES] = HOMEKIT_SETUP_SALT;
    static const uint8_t expected[SRP_VERIFIER_BYTES] = HOMEKIT_SETUP_VERIFIER;
    static const char user[] = "Pair-Setup";
    static const char code[] = HOMEKIT_TEST_SETUP_CODE;

    uint8_t v[SRP_VERIFIER_BYTES];
    HAP_srp_verifier(v, salt, (const uint8_t*) user, sizeof user - 1, (const uint8_t*) code, sizeof code - 1);
    HAPAssert(memcmp(v, expected, sizeof v) == 0);

    HAPAssert(strlen(HOMEKIT_SETUP_ID) == 4);
    printf("setup info OK (setup ID %s)\n", HOMEKIT_SETUP_ID);
    return 0;
}

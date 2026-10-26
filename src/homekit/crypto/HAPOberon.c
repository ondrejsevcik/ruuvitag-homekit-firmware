/**
 * @file HAPOberon.c
 * @brief HomeKit ADK crypto backend on top of Nordic's nrf_oberon library.
 *
 * nrf_oberon ships with nRF5 SDK 15.3 as a prebuilt Cortex-M4 library and
 * provides every primitive HAP-BLE needs (SRP-6a 3072, Ed25519, X25519,
 * ChaCha20-Poly1305, HKDF-SHA512, SHA-1/256/512).
 *
 * Only the functions referenced by the BLE transport are implemented.
 * IP-only primitives (AES-CTR, PBKDF2, HMAC-SHA1) are intentionally left
 * undefined so that accidentally pulling them in fails at link time.
 *
 * Build with HAVE_CUSTOM_SINGLE_SHOT_CHACHA20_POLY1305 so that HAPBase+Crypto.c
 * does not implement the single-shot AEAD on top of a streaming API.
 */

#include "HAPPlatform.h"
#include "HAPCrypto.h"

#include "occ_chacha20_poly1305.h"
#include "occ_curve25519.h"
#include "occ_ed25519.h"
#include "occ_hkdf_sha512.h"
#include "occ_sha1.h"
#include "occ_sha256.h"
#include "occ_sha512.h"
#include "occ_srp.h"

#ifndef HAVE_CUSTOM_SINGLE_SHOT_CHACHA20_POLY1305
#error "Define HAVE_CUSTOM_SINGLE_SHOT_CHACHA20_POLY1305 when building with the Oberon crypto backend."
#endif

HAP_STATIC_ASSERT(ED25519_PUBLIC_KEY_BYTES == occ_ed25519_PUBLIC_KEY_BYTES, ed25519_pk);
HAP_STATIC_ASSERT(ED25519_SECRET_KEY_BYTES == occ_ed25519_SECRET_KEY_BYTES, ed25519_sk);
HAP_STATIC_ASSERT(ED25519_BYTES == occ_ed25519_BYTES, ed25519_sig);
HAP_STATIC_ASSERT(X25519_SCALAR_BYTES == occ_curve25519_SCALAR_BYTES, x25519_scalar);
HAP_STATIC_ASSERT(X25519_BYTES == occ_curve25519_BYTES, x25519);
HAP_STATIC_ASSERT(CHACHA20_POLY1305_KEY_BYTES == occ_chacha20_poly1305_KEY_BYTES, chacha_key);
HAP_STATIC_ASSERT(CHACHA20_POLY1305_TAG_BYTES == occ_chacha20_poly1305_TAG_BYTES, chacha_tag);
HAP_STATIC_ASSERT(CHACHA20_POLY1305_NONCE_BYTES_MAX == occ_chacha20_poly1305_NONCE_BYTES_MAX, chacha_nonce);
HAP_STATIC_ASSERT(SRP_SALT_BYTES == occ_srp_SALT_BYTES, srp_salt);
HAP_STATIC_ASSERT(SRP_VERIFIER_BYTES == occ_srp_VERIFIER_BYTES, srp_verifier);
HAP_STATIC_ASSERT(SRP_SECRET_KEY_BYTES == occ_srp_SECRET_KEY_BYTES, srp_secret);
HAP_STATIC_ASSERT(SRP_PUBLIC_KEY_BYTES == occ_srp_PUBLIC_KEY_BYTES, srp_public);
HAP_STATIC_ASSERT(SRP_SCRAMBLING_PARAMETER_BYTES == occ_srp_SCRAMBLING_PARAMETER_BYTES, srp_u);
HAP_STATIC_ASSERT(SRP_PREMASTER_SECRET_BYTES == occ_srp_PREMASTER_SECRET_BYTES, srp_premaster);
HAP_STATIC_ASSERT(SRP_SESSION_KEY_BYTES == occ_srp_SESSION_KEY_BYTES, srp_session);
HAP_STATIC_ASSERT(SRP_PROOF_BYTES == occ_srp_PROOF_BYTES, srp_proof);
HAP_STATIC_ASSERT(SHA1_BYTES == occ_sha1_BYTES, sha1);
HAP_STATIC_ASSERT(SHA256_BYTES == occ_sha256_BYTES, sha256);
HAP_STATIC_ASSERT(SHA512_BYTES == occ_sha512_BYTES, sha512);

void HAP_ed25519_public_key(uint8_t pk[ED25519_PUBLIC_KEY_BYTES], const uint8_t sk[ED25519_SECRET_KEY_BYTES]) {
    occ_ed25519_public_key(pk, sk);
}

void HAP_ed25519_sign(
        uint8_t sig[ED25519_BYTES],
        const uint8_t* m,
        size_t m_len,
        const uint8_t sk[ED25519_SECRET_KEY_BYTES],
        const uint8_t pk[ED25519_PUBLIC_KEY_BYTES]) {
    occ_ed25519_sign(sig, m, m_len, sk, pk);
}

int HAP_ed25519_verify(
        const uint8_t sig[ED25519_BYTES],
        const uint8_t* m,
        size_t m_len,
        const uint8_t pk[ED25519_PUBLIC_KEY_BYTES]) {
    // Oberon and the ADK agree: 0 = valid, -1 = invalid.
    return occ_ed25519_verify(sig, m, m_len, pk) ? -1 : 0;
}

void HAP_X25519_scalarmult_base(uint8_t r[X25519_BYTES], const uint8_t n[X25519_SCALAR_BYTES]) {
    occ_curve25519_scalarmult_base(r, n);
}

void HAP_X25519_scalarmult(
        uint8_t r[X25519_BYTES],
        const uint8_t n[X25519_SCALAR_BYTES],
        const uint8_t p[X25519_BYTES]) {
    occ_curve25519_scalarmult(r, n, p);
}

void HAP_chacha20_poly1305_encrypt_aad(
        uint8_t tag[CHACHA20_POLY1305_TAG_BYTES],
        uint8_t* c,
        const uint8_t* m,
        size_t m_len,
        const uint8_t* a,
        size_t a_len,
        const uint8_t* n,
        size_t n_len,
        const uint8_t k[CHACHA20_POLY1305_KEY_BYTES]) {
    HAPPrecondition(n_len <= CHACHA20_POLY1305_NONCE_BYTES_MAX);
    if (a_len > 0) {
        occ_chacha20_poly1305_encrypt_aad(tag, c, m, m_len, a, a_len, n, n_len, k);
    } else {
        occ_chacha20_poly1305_encrypt(tag, c, m, m_len, n, n_len, k);
    }
}

int HAP_chacha20_poly1305_decrypt_aad(
        const uint8_t tag[CHACHA20_POLY1305_TAG_BYTES],
        uint8_t* m,
        const uint8_t* c,
        size_t c_len,
        const uint8_t* a,
        size_t a_len,
        const uint8_t* n,
        size_t n_len,
        const uint8_t k[CHACHA20_POLY1305_KEY_BYTES]) {
    HAPPrecondition(n_len <= CHACHA20_POLY1305_NONCE_BYTES_MAX);
    int e;
    if (a_len > 0) {
        e = occ_chacha20_poly1305_decrypt_aad(tag, m, c, c_len, a, a_len, n, n_len, k);
    } else {
        e = occ_chacha20_poly1305_decrypt(tag, m, c, c_len, n, n_len, k);
    }
    // Oberon returns 1 on authentication failure, the ADK expects -1.
    return e ? -1 : 0;
}

void HAP_srp_verifier(
        uint8_t v[SRP_VERIFIER_BYTES],
        const uint8_t salt[SRP_SALT_BYTES],
        const uint8_t* user,
        size_t user_len,
        const uint8_t* pass,
        size_t pass_len) {
    occ_srp_verifier(v, salt, user, user_len, pass, pass_len);
}

void HAP_srp_public_key(
        uint8_t pub_b[SRP_PUBLIC_KEY_BYTES],
        const uint8_t priv_b[SRP_SECRET_KEY_BYTES],
        const uint8_t v[SRP_VERIFIER_BYTES]) {
    occ_srp_public_key(pub_b, priv_b, v);
}

void HAP_srp_scrambling_parameter(
        uint8_t u[SRP_SCRAMBLING_PARAMETER_BYTES],
        const uint8_t pub_a[SRP_PUBLIC_KEY_BYTES],
        const uint8_t pub_b[SRP_PUBLIC_KEY_BYTES]) {
    occ_srp_scrambling_parameter(u, pub_a, pub_b);
}

int HAP_srp_premaster_secret(
        uint8_t s[SRP_PREMASTER_SECRET_BYTES],
        const uint8_t pub_a[SRP_PUBLIC_KEY_BYTES],
        const uint8_t priv_b[SRP_SECRET_KEY_BYTES],
        const uint8_t u[SRP_SCRAMBLING_PARAMETER_BYTES],
        const uint8_t v[SRP_VERIFIER_BYTES]) {
    // Oberon and the ADK agree: 0 = valid public key, 1 = illegal public key.
    return occ_srp_premaster_secret(s, pub_a, priv_b, u, v) ? 1 : 0;
}

void HAP_srp_session_key(uint8_t k[SRP_SESSION_KEY_BYTES], const uint8_t s[SRP_PREMASTER_SECRET_BYTES]) {
    occ_srp_session_key(k, s);
}

void HAP_srp_proof_m1(
        uint8_t m1[SRP_PROOF_BYTES],
        const uint8_t* user,
        size_t user_len,
        const uint8_t salt[SRP_SALT_BYTES],
        const uint8_t pub_a[SRP_PUBLIC_KEY_BYTES],
        const uint8_t pub_b[SRP_PUBLIC_KEY_BYTES],
        const uint8_t k[SRP_SESSION_KEY_BYTES]) {
    occ_srp_proof_m1(m1, user, user_len, salt, pub_a, pub_b, k);
}

void HAP_srp_proof_m2(
        uint8_t m2[SRP_PROOF_BYTES],
        const uint8_t pub_a[SRP_PUBLIC_KEY_BYTES],
        const uint8_t m1[SRP_PROOF_BYTES],
        const uint8_t k[SRP_SESSION_KEY_BYTES]) {
    occ_srp_proof_m2(m2, pub_a, m1, k);
}

void HAP_sha1(uint8_t md[SHA1_BYTES], const uint8_t* data, size_t size) {
    occ_sha1(md, data, size);
}

void HAP_sha256(uint8_t md[SHA256_BYTES], const uint8_t* data, size_t size) {
    occ_sha256(md, data, size);
}

void HAP_sha512(uint8_t md[SHA512_BYTES], const uint8_t* data, size_t size) {
    occ_sha512(md, data, size);
}

void HAP_hkdf_sha512(
        uint8_t* r,
        size_t r_len,
        const uint8_t* key,
        size_t key_len,
        const uint8_t* salt,
        size_t salt_len,
        const uint8_t* info,
        size_t info_len) {
    HAPPrecondition(r_len <= occ_hkdf_sha512_LENGTH_MAX);
    HAPPrecondition(salt_len <= occ_hkdf_sha512_SALT_LENGTH_MAX);
    occ_hkdf_sha512(r, r_len, key, key_len, salt, salt_len, info, info_len);
}

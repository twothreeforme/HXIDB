// Client half of the identity derivation (server/internal/oprf): a 2HashDH oblivious PRF on ristretto255.
//
// The reporter identity is derived from something stable about the player WITHOUT the server ever seeing it:
//   P = HashToPoint(input)      r = random scalar      send A = r*P  (uniformly random to the server)
//   server returns B = k*A      N = r^-1 * B = k*P     seed = H(input, N)  ->  Ed25519 key pair
// Nobody can compute the seed without the server's key k, and each guess of the input costs one rate-limited request.
// Nothing here touches the disk. Domain strings must match the server; test/oprf_test.cpp checks a shared vector.
#pragma once
#include <string>
#include "http.h"

#define SODIUM_STATIC
#define SODIUM_EXPORT
#include "../third_party/libsodium/include/sodium.h"

namespace hxidb {

inline const char* kOprfDomainPoint = "HXIDB-OPRF-v1|point|";
inline const char* kOprfDomainOut   = "HXIDB-OPRF-v1|out|";

// A = r*HashToPoint(input). Returns false if the result is not a usable element.
inline bool OprfBlind(const std::string& input, const uint8_t r[32], uint8_t A[32])
{
    if (sodium_init() < 0) return false;
    crypto_hash_sha512_state st;
    uint8_t h[64], p[32];
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, (const uint8_t*)kOprfDomainPoint, strlen(kOprfDomainPoint));
    crypto_hash_sha512_update(&st, (const uint8_t*)input.data(), input.size());
    crypto_hash_sha512_final(&st, h);
    if (crypto_core_ristretto255_from_hash(p, h) != 0) return false;
    return crypto_scalarmult_ristretto255(A, r, p) == 0;
}

// seed = H(input, r^-1 * B). Returns false if B is not a valid element.
inline bool OprfFinish(const std::string& input, const uint8_t r[32], const uint8_t B[32], uint8_t seed[32])
{
    if (sodium_init() < 0) return false;
    uint8_t rinv[32], n[32];
    if (crypto_core_ristretto255_scalar_invert(rinv, r) != 0) return false;
    if (crypto_scalarmult_ristretto255(n, rinv, B) != 0) return false;
    crypto_hash_sha512_state st;
    uint8_t h[64];
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, (const uint8_t*)kOprfDomainOut, strlen(kOprfDomainOut));
    crypto_hash_sha512_update(&st, (const uint8_t*)input.data(), input.size());
    crypto_hash_sha512_update(&st, n, 32);
    crypto_hash_sha512_final(&st, h);
    memcpy(seed, h, 32);
    sodium_memzero(rinv, sizeof(rinv));
    sodium_memzero(n, sizeof(n));
    sodium_memzero(h, sizeof(h));
    return true;
}

// One round trip to the server. httpStatus (optional) reports 0 for a network failure, 429 when rate-limited, 404 when
// the server has the derivation switched off.
inline bool OprfDerive(const std::string& baseUrl, const std::string& input, uint8_t seed[32], int* httpStatus = nullptr, uint32_t purpose = 0)
{
    if (httpStatus) *httpStatus = 0;
    if (sodium_init() < 0) return false;
    uint8_t r[32], A[32];
    crypto_core_ristretto255_scalar_random(r);
    if (!OprfBlind(input, r, A)) return false;
    PbWriter w;
    w.Raw(1, A, 32);
    if (purpose) w.U(2, purpose);
    HttpResult res = HttpPost(baseUrl, "/v1/oprf", w.Data());
    if (httpStatus) *httpStatus = res.status;
    bool ok = false;
    // OprfResponse { bytes evaluated = 1 }: tag 0x0A, length 32, 32 bytes.
    if (res.status == 200 && res.body.size() == 34 && res.body[0] == 0x0A && res.body[1] == 32)
        ok = OprfFinish(input, r, res.body.data() + 2, seed);
    sodium_memzero(r, sizeof(r));
    return ok;
}

} // namespace hxidb

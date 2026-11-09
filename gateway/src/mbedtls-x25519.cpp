#include "mbedtls-x25519.h"

#include <esp_system.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>
#include <string.h>

#include "secure-wipe.h"

namespace wattlink {

namespace {

static_assert(kPublicKeySize == 32 && kSharedSecretSize == 32, "X25519 keys and secrets are 32 bytes");

int fillRandom(void *, unsigned char *buffer, size_t length) {
  esp_fill_random(buffer, length);
  return 0;
}

// The clamping of RFC 7748, section 5.
void clamp(uint8_t key[kPublicKeySize]) {
  key[0] &= 248;
  key[31] &= 127;
  key[31] |= 64;
}

// Multiplies the point with the u coordinate peer (or the base point when peer is null) by the clamped scalar, and writes the u coordinate of the
// result. The random function of the chip only blinds the multiplication against timing attacks.
bool multiply(const uint8_t scalar[kPublicKeySize], const uint8_t *peer, uint8_t out[kPublicKeySize]) {
  uint8_t clamped[kPublicKeySize];
  memcpy(clamped, scalar, sizeof(clamped));
  clamp(clamped);

  mbedtls_ecp_group group;
  mbedtls_ecp_point base;
  mbedtls_ecp_point result;
  mbedtls_mpi d;
  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point_init(&base);
  mbedtls_ecp_point_init(&result);
  mbedtls_mpi_init(&d);

  bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_CURVE25519) == 0 && mbedtls_mpi_read_binary_le(&d, clamped, sizeof(clamped)) == 0;
  if (ok && peer == nullptr) {
    ok = mbedtls_ecp_copy(&base, &group.G) == 0;
  } else if (ok) {
    // The RFC says that the top bit of the u coordinate is ignored. A value that is still not below the prime is refused by mbedTLS.
    uint8_t u[kPublicKeySize];
    memcpy(u, peer, sizeof(u));
    u[31] &= 0x7F;
    ok = mbedtls_mpi_read_binary_le(&base.X, u, sizeof(u)) == 0 && mbedtls_mpi_lset(&base.Z, 1) == 0;
  }
  ok = ok && mbedtls_ecp_mul(&group, &result, &d, &base, fillRandom, nullptr) == 0 &&
       mbedtls_mpi_write_binary_le(&result.X, out, kPublicKeySize) == 0;
  if (!ok) {
    memset(out, 0, kPublicKeySize);
  }

  // The free functions wipe the numbers.
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&result);
  mbedtls_ecp_point_free(&base);
  mbedtls_ecp_group_free(&group);
  wipe(clamped, sizeof(clamped));
  return ok;
}

}  // namespace

bool mbedtlsX25519PublicKey(const uint8_t privateKey[kPublicKeySize], uint8_t publicKey[kPublicKeySize]) {
  return multiply(privateKey, nullptr, publicKey);
}

bool mbedtlsX25519Generate(void *, uint8_t privateKey[kPublicKeySize], uint8_t publicKey[kPublicKeySize]) {
  esp_fill_random(privateKey, kPublicKeySize);
  clamp(privateKey);
  if (!mbedtlsX25519PublicKey(privateKey, publicKey)) {
    wipe(privateKey, kPublicKeySize);
    return false;
  }
  return true;
}

bool mbedtlsX25519Agree(void *, const uint8_t privateKey[kPublicKeySize], const uint8_t peerPublicKey[kPublicKeySize],
                        uint8_t shared[kSharedSecretSize]) {
  return multiply(privateKey, peerPublicKey, shared);
}

}  // namespace wattlink

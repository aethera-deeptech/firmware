#include "mbedtls-crypto.h"

#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

namespace wattlink {

bool mbedtlsHmacSha256(void *, const uint8_t *key, size_t keyLength, const uint8_t *data, size_t dataLength, uint8_t out[kSha256Size]) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info != nullptr && mbedtls_md_hmac(info, key, keyLength, data, dataLength, out) == 0;
}

bool mbedtlsSha256(void *, const uint8_t *data, size_t length, uint8_t out[kSha256Size]) {
  return mbedtls_sha256_ret(data, length, out, 0) == 0;
}

bool mbedtlsAesGcmSeal(void *, const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad, size_t aadLength,
                       const uint8_t *plain, size_t length, uint8_t *cipher, uint8_t tag[kTagSize]) {
  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  const bool ok = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, kAesKeySize * 8) == 0 &&
                  mbedtls_gcm_crypt_and_tag(&context, MBEDTLS_GCM_ENCRYPT, length, nonce, kNonceSize, aad, aadLength, plain, cipher, kTagSize,
                                            tag) == 0;
  // mbedtls_gcm_free wipes the key schedule.
  mbedtls_gcm_free(&context);
  return ok;
}

bool mbedtlsAesGcmOpen(void *, const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad, size_t aadLength,
                       const uint8_t *cipher, size_t length, const uint8_t tag[kTagSize], uint8_t *plain) {
  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  const bool ok = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, kAesKeySize * 8) == 0 &&
                  mbedtls_gcm_auth_decrypt(&context, length, nonce, kNonceSize, aad, aadLength, tag, kTagSize, cipher, plain) == 0;
  mbedtls_gcm_free(&context);
  return ok;
}

}  // namespace wattlink

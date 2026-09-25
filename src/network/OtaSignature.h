#pragma once

#include <cstddef>
#include <cstdint>

#include "OtaUpdatePublicKey.h"

#ifndef SIMULATOR
#if !defined(INKADEMIC_REQUIRE_SIGNED_OTA) || INKADEMIC_REQUIRE_SIGNED_OTA != 1
#error "Hardware OTA builds must require signed firmware"
#endif
#if !defined(HAVE_ED25519) || !defined(HAVE_ED25519_VERIFY) || !defined(HAVE_ED25519_KEY_IMPORT)
#error "Hardware OTA builds must compile the Ed25519 verifier"
#endif
#include <wolfssl/wolfcrypt/ed25519.h>
#ifndef WOLFSSL_SHA512
#error "Ed25519 requires SHA-512"
#endif
#endif

namespace ota_signature {
inline bool verify(const uint8_t* digest, const uint8_t* signature, size_t length) {
#ifdef SIMULATOR
  (void)digest;
  (void)signature;
  (void)length;
  return false;
#else
  if (!digest || !signature || length != 64) return false;
  // Bounded key context (112 bytes on host); no persistent SHA context enabled.
  ed25519_key key;
  if (wc_ed25519_init(&key) != 0) return false;
  const int imported = wc_ed25519_import_public(inkademic_ota::kEd25519PublicKey, 32, &key);
  int valid = 0;
  const int result = imported == 0 ? wc_ed25519_verify_msg(signature, 64, digest, 32, &valid, &key) : -1;
  wc_ed25519_free(&key);
  return result == 0 && valid == 1;
#endif
}
}  // namespace ota_signature

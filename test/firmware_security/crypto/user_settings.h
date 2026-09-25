#pragma once
// Host-only platform configuration. Compile the same wolfSSL Ed25519 sources
// used by firmware; no replacement verifier or production private key.
#define WOLFCRYPT_ONLY
#define SINGLE_THREADED
#define WOLFSSL_SMALL_STACK
#define NO_ED25519_SIGN
#define NO_ED25519_MAKE_KEY
#define NO_RSA
#define NO_DH
#define NO_DSA
#define NO_AES
#define NO_DES3
#define NO_RC4
#define NO_MD5
#define NO_SHA
#define NO_SHA256
#define NO_HMAC
#define NO_PWDBASED
#define NO_ASN
#define WC_NO_RNG
#define WC_NO_HASHDRBG
#define NO_WRITEV

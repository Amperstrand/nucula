#pragma once
// Minimal mbedtls config for the host test build: exactly what the
// nucula core uses (sha256, HMAC-MD, PBKDF2, base64) and nothing else —
// the stock 3.x default config also builds SHA3 and the PSA crypto
// core, whose symbols the test build does not link.
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PKCS5_C

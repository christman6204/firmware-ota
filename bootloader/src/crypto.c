#include "crypto.h"
#include <string.h>

/* mbedtls V2.28 LTS */
#include "mbedtls/aes.h"
#include "mbedtls/md.h"

const uint8_t BOOT_MASTER_AES_KEY[32]  = {0};
const uint8_t BOOT_MASTER_HMAC_KEY[32] = {0};

static mbedtls_aes_context aes_ctx;
static uint8_t ctr_iv[16];
static mbedtls_md_context_t hmac_ctx;

void crypto_aes_ctr_init(const uint8_t key[32], const uint8_t iv[16]) {
    memcpy(ctr_iv, iv, 16);
    mbedtls_aes_setkey_enc(&aes_ctx, key, 256);
}

void crypto_aes_ctr_crypt(const uint8_t *in, uint8_t *out, uint32_t len) {
    size_t nc_off = 0;
    uint8_t stream_block[16];
    mbedtls_aes_crypt_ctr(&aes_ctx, len, &nc_off, ctr_iv, stream_block, in, out);
}

void crypto_hmac_sha256_init(const uint8_t key[32]) {
    mbedtls_md_init(&hmac_ctx);
    mbedtls_md_setup(&hmac_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&hmac_ctx, key, 32);
}

void crypto_hmac_sha256_update(const uint8_t *data, uint32_t len) {
    mbedtls_md_hmac_update(&hmac_ctx, data, len);
}

void crypto_hmac_sha256_final(uint8_t hmac[32]) {
    mbedtls_md_hmac_finish(&hmac_ctx, hmac);
    mbedtls_md_free(&hmac_ctx);
}

int crypto_hmac_sha256_verify(const uint8_t *data, uint32_t len,
                               const uint8_t expected[32]) {
    uint8_t calc[32];
    crypto_hmac_sha256_init(BOOT_MASTER_HMAC_KEY);
    crypto_hmac_sha256_update(data, len);
    crypto_hmac_sha256_final(calc);
    return (memcmp(calc, expected, 32) == 0) ? 0 : -1;
}

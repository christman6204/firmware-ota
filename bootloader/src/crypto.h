#ifndef CRYPTO_H
#define CRYPTO_H
#include <stdint.h>

void crypto_aes_ctr_init(const uint8_t key[32], const uint8_t iv[16]);
void crypto_aes_ctr_crypt(const uint8_t *in, uint8_t *out, uint32_t len);

#define CRYPTO_HMAC_SIZE 32
void crypto_hmac_sha256_init(const uint8_t key[32]);
void crypto_hmac_sha256_update(const uint8_t *data, uint32_t len);
void crypto_hmac_sha256_final(uint8_t hmac[CRYPTO_HMAC_SIZE]);
int  crypto_hmac_sha256_verify(const uint8_t *data, uint32_t len,
                                const uint8_t expected_hmac[CRYPTO_HMAC_SIZE]);

extern const uint8_t BOOT_MASTER_AES_KEY[32];
extern const uint8_t BOOT_MASTER_HMAC_KEY[32];
#endif

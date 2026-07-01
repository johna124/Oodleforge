#ifndef AES_H
#define AES_H

#include <stdint.h>
#include <stddef.h>

#define AES256 1
#define CBC 1
#define ECB 1

#define AES_BLOCKLEN 16
#define AES_KEYLEN 240
#define AES_NUMBER_OF_ROUNDS 14

#ifdef __cplusplus
extern "C" {
#endif

struct AES_ctx {
    uint8_t RoundKey[AES_KEYLEN];
    uint8_t Iv[AES_BLOCKLEN];
};

void AES_init_ctx(struct AES_ctx* ctx, const uint8_t* key);
void AES_init_ctx_iv(struct AES_ctx* ctx, const uint8_t* key, const uint8_t* iv);
void AES_ctx_set_iv(struct AES_ctx* ctx, const uint8_t* iv);

void AES_ECB_encrypt(const struct AES_ctx* ctx, uint8_t* buf);
void AES_ECB_decrypt(const struct AES_ctx* ctx, uint8_t* buf);

// FIX: Changed uint32_t to size_t to match aes.c implementation
void AES_CBC_encrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);
void AES_CBC_decrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);

#ifdef __cplusplus
}
#endif

#endif // AES_H

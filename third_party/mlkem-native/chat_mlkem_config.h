/*
 * Argonaut Chat's configuration of mlkem-native (MLK_CONFIG_FILE): see
 * mlkem/mlkem_native_config.h for what each option means.
 */
#ifndef MLK_CONFIG_H
#define MLK_CONFIG_H

/* ML-KEM-1024, as Signal's PQXDH. */
#define MLK_CONFIG_PARAMETER_SET 1024
#define MLK_CONFIG_NAMESPACE_PREFIX chat_mlkem
/* One compilation unit (mlkem_native.c): internal functions stay static. */
#define MLK_CONFIG_INTERNAL_API_QUALIFIER static

/* Randomness from libsodium (src/core/MlKem.cpp). Not mlkem-native's
 * default `randombytes`: libsodium exports a function of that name. */
#define MLK_CONFIG_CUSTOM_RANDOMBYTES
#if !defined(__ASSEMBLER__)
#include <stddef.h>
#include <stdint.h>
#include "src/sys.h"
#ifdef __cplusplus
extern "C" {
#endif
int chat_mlkem_randombytes(uint8_t *out, size_t length);
#ifdef __cplusplus
}
#endif
static MLK_INLINE int mlk_randombytes(uint8_t *ptr, size_t len)
{
    return chat_mlkem_randombytes(ptr, len);
}
#endif

#endif /* MLK_CONFIG_H */

/* Randomness for mlkem-native (chat_mlkem_config.h): libsodium's, the same
 * as every other key here. */
#include <sodium.h>

#include <stddef.h>
#include <stdint.h>

int chat_mlkem_randombytes(uint8_t *out, size_t length)
{
    if (sodium_init() < 0)
        return -1;
    randombytes_buf(out, length);
    return 0;
}

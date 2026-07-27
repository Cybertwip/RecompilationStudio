#include <stddef.h>
#include <stdint.h>

void *memset(void *destination, int value, size_t size)
{
    unsigned char *out = (unsigned char *)destination;
    for (size_t i = 0; i < size; ++i)
        out[i] = (unsigned char)value;
    return destination;
}

void *memcpy(void *destination, const void *source, size_t size)
{
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    for (size_t i = 0; i < size; ++i)
        out[i] = in[i];
    return destination;
}

void *memmove(void *destination, const void *source, size_t size)
{
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    if (out <= in || out >= in + size) {
        for (size_t i = 0; i < size; ++i)
            out[i] = in[i];
    } else {
        for (size_t i = size; i != 0; --i)
            out[i - 1] = in[i - 1];
    }
    return destination;
}

void __aeabi_memclr(void *destination, size_t size)
{
    (void)memset(destination, 0, size);
}

void __aeabi_memclr4(void *destination, size_t size)
{
    (void)memset(destination, 0, size);
}

void __aeabi_memclr8(void *destination, size_t size)
{
    (void)memset(destination, 0, size);
}

void __aeabi_memcpy(void *destination, const void *source, size_t size)
{
    (void)memcpy(destination, source, size);
}

void __aeabi_memcpy4(void *destination, const void *source, size_t size)
{
    (void)memcpy(destination, source, size);
}

void __aeabi_memcpy8(void *destination, const void *source, size_t size)
{
    (void)memcpy(destination, source, size);
}

void __aeabi_memmove(void *destination, const void *source, size_t size)
{
    (void)memmove(destination, source, size);
}

#pragma once
#include <cstddef>
#include <string>
inline int mbedtls_base64_decode(unsigned char* out, size_t cap, size_t* n, const unsigned char* s,
                                 size_t len) {
    std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned bits = 0, value = 0;
    *n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '=')
            break;
        auto pos = chars.find(s[i]);
        if (pos == std::string::npos)
            return -1;
        value = (value << 6) | pos;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (*n == cap)
                return -1;
            out[(*n)++] = (value >> bits) & 255;
        }
    }
    return 0;
}
inline int mbedtls_base64_encode(unsigned char* out, size_t cap, size_t* n,
                                 const unsigned char* src, size_t len) {
    const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    *n = 4 * ((len + 2) / 3);
    if (cap <= *n)
        return -1;
    size_t at = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = unsigned(src[i]) << 16;
        if (i + 1 < len)
            v |= unsigned(src[i + 1]) << 8;
        if (i + 2 < len)
            v |= src[i + 2];
        out[at++] = table[(v >> 18) & 63];
        out[at++] = table[(v >> 12) & 63];
        out[at++] = i + 1 < len ? table[(v >> 6) & 63] : '=';
        out[at++] = i + 2 < len ? table[v & 63] : '=';
    }
    out[at] = 0;
    return 0;
}

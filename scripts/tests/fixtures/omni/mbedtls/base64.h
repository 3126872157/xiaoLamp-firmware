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
inline int mbedtls_base64_encode(unsigned char*, size_t, size_t*, const unsigned char*, size_t) {
    return -1;
}

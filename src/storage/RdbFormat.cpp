#include "RdbFormat.h"
#include <arpa/inet.h>
#include <cstring>

namespace minidb {

uint32_t RdbFormat::crc32c(const uint8_t* data, size_t len) {
    // 简化的 CRC 实现，使用多项式 0x1EDC6F41
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        crc ^= byte;
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82F63B78; // CRC-32C 反向多项式
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xffffffff;
}

std::vector<uint8_t> RdbFormat::encode_varint(uint32_t value) {
    std::vector<uint8_t> result;
    while (value >= 128) {
        result.push_back((value & 0x7f) | 0x80);
        value >>= 7;
    }
    result.push_back(value & 0x7f);
    return result;
}

std::pair<uint32_t, size_t> RdbFormat::decode_varint(const uint8_t* data,
                                                     size_t max_len) {
    uint32_t value = 0;
    size_t bytes_read = 0;

    // P0: 限制最大5字节，防止移位超过32位
    constexpr size_t kMaxVarintBytes = 5;
    const size_t limit =
        (max_len < kMaxVarintBytes) ? max_len : kMaxVarintBytes;

    for (size_t i = 0; i < limit; ++i) {
        uint8_t byte = data[i];

        // P0: 在移位前检查是否会超出32位
        // i=0: shift=0, i=1: shift=7, i=2: shift=14, i=3: shift=21, i=4:
        // shift=28 i=5会导致shift=35，超过32位
        if (i * 7 >= 32) {
            // 超长varint，返回失败（bytes_read=0表示错误）
            return {0, 0};
        }

        value |= (static_cast<uint32_t>(byte & 0x7f) << (i * 7));
        bytes_read++;

        // AI-014 P1 FIX: Validate 5th byte BEFORE checking continuation bit
        // This ensures the check is reached regardless of continuation bit
        // state
        if (i == 4) {
            // uint32 varint: 5th byte contributes bits [28,31], only 4 bits
            // available Reject if:
            // 1. Still has continuation bit (would need 6th byte, causing UB
            // shift)
            // 2. Payload > 0x0f (would overflow 32 bits)
            if ((byte & 0x80) != 0) {
                return {0, 0}; // 6th byte would shift by 35
            }
            uint8_t payload = byte & 0x7f;
            if (payload > 0x0f) {
                return {0, 0}; // Value would exceed 0xFFFFFFFF
            }
        }

        if ((byte & 0x80) == 0) {
            break;
        }
    }

    // 如果读取了max_len字节但最后一个字节仍有continuation bit，这是错误
    if (bytes_read > 0 && bytes_read == limit && limit < kMaxVarintBytes) {
        if ((data[bytes_read - 1] & 0x80) != 0) {
            return {0, 0};
        }
    }

    return {value, bytes_read};
}

uint32_t RdbFormat::hton32(uint32_t value) {
    return htonl(value);
}

uint32_t RdbFormat::ntoh32(uint32_t value) {
    return ntohl(value);
}

uint64_t RdbFormat::hton64(uint64_t value) {
    return ((uint64_t)htonl(value >> 32)) | (((uint64_t)htonl(value)) << 32);
}

uint64_t RdbFormat::ntoh64(uint64_t value) {
    return ((uint64_t)ntohl(value >> 32)) | (((uint64_t)ntohl(value)) << 32);
}

} // namespace minidb

#include "base/Sha256.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace minidb {
namespace {

std::string hex_digest(std::string_view input) {
    const Sha256::Digest digest = Sha256::digest(input);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        result.push_back(kHex[byte >> 4u]);
        result.push_back(kHex[byte & 0x0fu]);
    }
    return result;
}

} // namespace

TEST(Sha256Test, KnownVectors) {
    EXPECT_EQ(
        hex_digest(""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(
        hex_digest("abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(
        hex_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256Test, MultiBlockPaddingBoundaries) {
    EXPECT_EQ(
        hex_digest(std::string(55, static_cast<char>(97))),
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    EXPECT_EQ(
        hex_digest(std::string(56, static_cast<char>(97))),
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    EXPECT_EQ(
        hex_digest(std::string(63, static_cast<char>(97))),
        "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    EXPECT_EQ(
        hex_digest(std::string(64, static_cast<char>(97))),
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    EXPECT_EQ(
        hex_digest(std::string(65, static_cast<char>(97))),
        "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
}

TEST(Sha256Test, LongBinaryInput) {
    std::string input;
    for (std::size_t index = 0; index < 1000; ++index) {
        input.push_back(static_cast<char>(index & 0xffu));
    }
    EXPECT_EQ(
        hex_digest(input),
        "a8af099bf2e878609558dbf69d8f88f4a31040a8cf84b549a0cfa912f12ffc3f");
}

} // namespace minidb

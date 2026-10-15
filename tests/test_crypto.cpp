// Copyright 2026 BackupTool Team. All rights reserved.

#include "core/crypto.h"

#include <gtest/gtest.h>

namespace {

TEST(CryptoTest, Sha256MatchesKnownVector) {
    const std::string input = "abc";
    std::vector<uint8_t> digest;
    ASSERT_TRUE(dbackup::crypto::sha256(
        reinterpret_cast<const uint8_t *>(input.data()), input.size(), digest));
    const std::vector<uint8_t> expected = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    EXPECT_EQ(digest, expected);
}

TEST(CryptoTest, Pbkdf2IsDeterministicAndProducesRequestedLength) {
    std::vector<uint8_t> salt(16, 0x5a);
    std::vector<uint8_t> first(32), second(32);
    ASSERT_TRUE(dbackup::crypto::pbkdf2Sha256("test-password", salt, 1000, first));
    ASSERT_TRUE(dbackup::crypto::pbkdf2Sha256("test-password", salt, 1000, second));
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.size(), 32u);
}

TEST(CryptoTest, Pbkdf2MatchesKnownSha256Vector) {
    std::vector<uint8_t> key(32);
    ASSERT_TRUE(dbackup::crypto::pbkdf2Sha256("password", {'s', 'a', 'l', 't'}, 1, key));
    const std::vector<uint8_t> expected = {
        0x12, 0x0f, 0xb6, 0xcf, 0xfc, 0xf8, 0xb3, 0x2c,
        0x43, 0xe7, 0x22, 0x52, 0x56, 0xc4, 0xf8, 0x37,
        0xa8, 0x65, 0x48, 0xc9, 0x2c, 0xcc, 0x35, 0x48,
        0x08, 0x05, 0x98, 0x7c, 0xb7, 0x0b, 0xe1, 0x7b};
    EXPECT_EQ(key, expected);
}

TEST(CryptoTest, AesGcmRoundTripAndRejectsModifiedTag) {
    const std::vector<uint8_t> plaintext = {'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    const std::vector<uint8_t> key(32, 0x31);
    std::vector<uint8_t> nonce, tag, ciphertext, restored;
    ASSERT_TRUE(dbackup::crypto::aes256Gcm(true, plaintext, key, nonce, tag, ciphertext));
    ASSERT_EQ(nonce.size(), 12u);
    ASSERT_EQ(tag.size(), 16u);
    ASSERT_TRUE(dbackup::crypto::aes256Gcm(false, ciphertext, key, nonce, tag, restored));
    EXPECT_EQ(restored, plaintext);

    tag[0] ^= 0x01;
    EXPECT_FALSE(dbackup::crypto::aes256Gcm(false, ciphertext, key, nonce, tag, restored));
}

TEST(CryptoTest, RejectsInvalidKeyAndNonceSizes) {
    const std::vector<uint8_t> input = {'x'};
    std::vector<uint8_t> nonce, tag, output;
    EXPECT_FALSE(dbackup::crypto::aes256Gcm(true, input, std::vector<uint8_t>(16), nonce, tag, output));
    const std::vector<uint8_t> key(32, 0x42);
    nonce.assign(11, 0);
    EXPECT_FALSE(dbackup::crypto::aes256Gcm(true, input, key, nonce, tag, output));
}

}  // namespace

#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <bcrypt.h>

namespace wf {
using Bytes = std::vector<unsigned char>;

struct Media {
    Bytes data;
    std::string ext;
    uint32_t version = 0, aesSize = 0, xorSize = 0, rawSize = 0, flag = 0;
};

inline std::string extension(const Bytes& b) {
    if (b.size() < 12) return {};
    const std::string_view head(reinterpret_cast<const char*>(b.data()), 12);
    if (head.substr(0, 4) == "\x89PNG") return "png";
    if (head.substr(0, 3) == "\xff\xd8\xff") return "jpg";
    if (head.substr(0, 6) == "GIF89a" || head.substr(0, 6) == "GIF87a") return "gif";
    if (head.substr(0, 4) == "RIFF" && head.substr(8, 4) == "WEBP") return "webp";
    if (head.substr(4, 4) == "ftyp") return "mp4";
    if (head.substr(0, 4) == "wxgf") return "wxgf";
    return {};
}

inline uint32_t readLe32(const Bytes& data, size_t at) {
    return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 |
        uint32_t(data[at + 2]) << 16 | uint32_t(data[at + 3]) << 24;
}

inline Bytes decryptAes(const Bytes& encrypted, ULONG length, std::string key) {
    struct Cipher {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;
        ~Cipher() {
            if (key) BCryptDestroyKey(key);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } cipher;
    Bytes result(length);
    ULONG written = 0;
    if (key.size() < 16 ||
        BCryptOpenAlgorithmProvider(&cipher.algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0 ||
        BCryptSetProperty(cipher.algorithm, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)), sizeof(BCRYPT_CHAIN_MODE_ECB), 0) < 0 ||
        BCryptGenerateSymmetricKey(cipher.algorithm, &cipher.key, nullptr, 0,
            reinterpret_cast<PUCHAR>(key.data()), 16, 0) < 0 ||
        BCryptDecrypt(cipher.key, const_cast<PUCHAR>(encrypted.data() + 15), length,
            nullptr, nullptr, 0, result.data(), length, &written, BCRYPT_BLOCK_PADDING) < 0)
        throw std::runtime_error("DAT AES decryption failed");
    result.resize(written);
    return result;
}

inline Media decrypt(const std::filesystem::path& file, unsigned char xorKey, const std::string& aesKey) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open DAT file");
    Bytes input{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    auto ext = extension(input);
    if (!ext.empty()) return Media{std::move(input), std::move(ext)};

    Media media;
    if (input.size() >= 6 && input[0] == 7 && input[1] == 8 && input[2] == 'V' &&
        (input[3] == '1' || input[3] == '2') && input[4] == 8 && input[5] == 7) {
        if (input.size() < 15) throw std::runtime_error("Truncated DAT header");
        media.version = input[3] - '0';
        media.aesSize = readLe32(input, 6);
        media.xorSize = readLe32(input, 10);
        media.flag = input[14];
        const uint64_t aligned = (uint64_t(media.aesSize) / 16 + 1) * 16;
        if (aligned > UINT32_MAX || aligned + media.xorSize > input.size() - 15)
            throw std::runtime_error("Invalid DAT payload lengths");
        media.data = decryptAes(input, static_cast<ULONG>(aligned), aesKey);
        if (media.data.size() != media.aesSize) throw std::runtime_error("Invalid DAT AES length");
        const auto tail = input.size() - media.xorSize;
        media.rawSize = static_cast<uint32_t>(tail - 15 - aligned);
        media.data.insert(media.data.end(), input.begin() + 15 + aligned, input.begin() + tail);
        for (size_t i = tail; i < input.size(); ++i) media.data.push_back(input[i] ^ xorKey);
    } else {
        for (auto& byte : input) byte ^= xorKey;
        media.data = std::move(input);
    }
    media.ext = extension(media.data);
    if (media.ext.empty()) throw std::runtime_error("Unknown DAT media type");
    return media;
}
}

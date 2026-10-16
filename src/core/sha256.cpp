#include "core/sha256.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <fstream>
#include <vector>

namespace core {

namespace {

constexpr size_t kDigestSize = 32;

std::string ToHex(const std::uint8_t* digest)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(kDigestSize * 2);
    for (size_t i = 0; i < kDigestSize; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0xF]);
    }
    return out;
}

// RAII wrapper around one CNG hash computation.
class Hasher {
public:
    Hasher()
    {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return;
        if (!BCRYPT_SUCCESS(BCryptCreateHash(m_alg, &m_hash, nullptr, 0, nullptr, 0, 0))) {
            m_hash = nullptr;
        }
    }
    ~Hasher()
    {
        if (m_hash != nullptr) BCryptDestroyHash(m_hash);
        if (m_alg != nullptr) BCryptCloseAlgorithmProvider(m_alg, 0);
    }
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

    bool Ok() const { return m_hash != nullptr; }

    bool Update(const std::uint8_t* data, size_t size)
    {
        while (size > 0) {
            const ULONG chunk = size > 0x40000000u ? 0x40000000u : static_cast<ULONG>(size);
            if (!BCRYPT_SUCCESS(BCryptHashData(m_hash, const_cast<PUCHAR>(data), chunk, 0))) return false;
            data += chunk;
            size -= chunk;
        }
        return true;
    }

    std::string Finish()
    {
        std::uint8_t digest[kDigestSize] = {};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(m_hash, digest, static_cast<ULONG>(kDigestSize), 0))) return {};
        return ToHex(digest);
    }

private:
    BCRYPT_ALG_HANDLE m_alg = nullptr;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
};

} // namespace

std::string Sha256Hex(std::span<const std::uint8_t> data)
{
    try {
        Hasher h;
        if (!h.Ok() || !h.Update(data.data(), data.size())) return {};
        return h.Finish();
    }
    catch (...) {
        return {};
    }
}

std::optional<std::string> Sha256FileHex(const std::filesystem::path& file)
{
    try {
        std::ifstream in(file, std::ios::binary);
        if (!in) return std::nullopt;

        Hasher h;
        if (!h.Ok()) return std::nullopt;

        std::vector<std::uint8_t> buf(1u << 20);
        while (in) {
            in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = in.gcount();
            if (got > 0 && !h.Update(buf.data(), static_cast<size_t>(got))) return std::nullopt;
        }
        if (in.bad()) return std::nullopt;

        std::string hex = h.Finish();
        if (hex.empty()) return std::nullopt;
        return hex;
    }
    catch (...) {
        return std::nullopt;
    }
}

} // namespace core

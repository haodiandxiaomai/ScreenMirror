#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#ifndef HUILANG_PROTECT_STRINGS
#define HUILANG_PROTECT_STRINGS 1
#endif

#if HUILANG_PROTECT_STRINGS
namespace hlprotect {

inline void SecureWipe(void* p, size_t bytes) noexcept {
    volatile unsigned char* v = static_cast<volatile unsigned char*>(p);
    while (bytes--) *v++ = 0;
}

constexpr uint32_t MixKey(uint32_t key, size_t index) noexcept {
    uint32_t x = key ^ static_cast<uint32_t>(index * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

template <size_t N, uint32_t KEY>
struct EncWideString {
    std::array<uint32_t, N> data{};

    constexpr EncWideString(const wchar_t (&s)[N]) : data{} {
        for (size_t i = 0; i < N; ++i) {
            data[i] = static_cast<uint32_t>(s[i]) ^ (MixKey(KEY, i) & 0x001FFFFFu);
        }
    }
};

template <size_t N, uint32_t KEY>
constexpr EncWideString<N, KEY> MakeWide(const wchar_t (&s)[N]) {
    return EncWideString<N, KEY>(s);
}

template <size_t N, uint32_t KEY>
const wchar_t* DecodeWide(const EncWideString<N, KEY>& enc) {
    struct Slot {
        std::wstring text;
        ~Slot() {
            if (!text.empty()) SecureWipe(text.data(), text.size() * sizeof(wchar_t));
        }
    };
    static constexpr size_t kRingSize = 32;
    thread_local std::array<Slot, kRingSize> ring{};
    thread_local size_t cursor = 0;

    cursor = (cursor + 1) % kRingSize;
    std::wstring& out = ring[cursor].text;
    if (!out.empty()) SecureWipe(out.data(), out.size() * sizeof(wchar_t));
    out.assign(N > 0 ? N - 1 : 0, L'\0');
    for (size_t i = 0; i + 1 < N; ++i) {
        out[i] = static_cast<wchar_t>(enc.data[i] ^ (MixKey(KEY, i) & 0x001FFFFFu));
    }
    return out.c_str();
}

template <size_t N, uint32_t KEY>
struct EncAnsiString {
    std::array<uint8_t, N> data{};

    constexpr EncAnsiString(const char (&s)[N]) : data{} {
        for (size_t i = 0; i < N; ++i) {
            data[i] = static_cast<uint8_t>(static_cast<unsigned char>(s[i]) ^ (MixKey(KEY, i) & 0xFFu));
        }
    }
};

template <size_t N, uint32_t KEY>
constexpr EncAnsiString<N, KEY> MakeAnsi(const char (&s)[N]) {
    return EncAnsiString<N, KEY>(s);
}

template <size_t N, uint32_t KEY>
const char* DecodeAnsi(const EncAnsiString<N, KEY>& enc) {
    struct Slot {
        std::string text;
        ~Slot() {
            if (!text.empty()) SecureWipe(text.data(), text.size());
        }
    };
    static constexpr size_t kRingSize = 32;
    thread_local std::array<Slot, kRingSize> ring{};
    thread_local size_t cursor = 0;

    cursor = (cursor + 1) % kRingSize;
    std::string& out = ring[cursor].text;
    if (!out.empty()) SecureWipe(out.data(), out.size());
    out.assign(N > 0 ? N - 1 : 0, '\0');
    for (size_t i = 0; i + 1 < N; ++i) {
        out[i] = static_cast<char>(enc.data[i] ^ (MixKey(KEY, i) & 0xFFu));
    }
    return out.c_str();
}

} // namespace hlprotect

#define HLPROTECT_KEY_ (static_cast<uint32_t>(0xA5A5A5A5u ^ (__LINE__ * 0x9E3779B9u) ^ (__COUNTER__ * 0x85EBCA6Bu)))
#define HLW(s) ([]() -> const wchar_t* { static constexpr auto enc = ::hlprotect::MakeWide<sizeof(s) / sizeof(wchar_t), HLPROTECT_KEY_>(s); return ::hlprotect::DecodeWide(enc); }())
#define HLA(s) ([]() -> const char* { static constexpr auto enc = ::hlprotect::MakeAnsi<sizeof(s) / sizeof(char), HLPROTECT_KEY_>(s); return ::hlprotect::DecodeAnsi(enc); }())

#else
#define HLW(s) (s)
#define HLA(s) (s)
#endif

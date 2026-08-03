#include "hl_common.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>


CommandResult RunCommandCapture(const std::string& cmd) {
    CommandResult result;

    std::string cmdLine = cmd;

    // 以前给 _popen 用的 shell 重定向；CreateProcess 不需要，stderr 直接接到同一个 pipe。
    const std::string redir = " 2>&1";
    if (cmdLine.size() >= redir.size() &&
        cmdLine.compare(cmdLine.size() - redir.size(), redir.size(), redir) == 0) {
        cmdLine.resize(cmdLine.size() - redir.size());
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        result.output = "CreatePipe failed";
        return result;
    }

    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr,
        mutableCmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    CloseHandle(writePipe);

    if (!ok) {
        DWORD err = GetLastError();
        char msg[128];
        std::snprintf(msg, sizeof(msg), "CreateProcess failed: %lu", static_cast<unsigned long>(err));
        result.output = msg;
        CloseHandle(readPipe);
        return result;
    }

    char buf[512];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        result.output += buf;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    if (GetExitCodeProcess(pi.hProcess, &exitCode)) {
        result.rc = static_cast<int>(exitCode);
    } else {
        result.rc = -1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);

    return result;
}

std::string TrimAscii(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    if (i > 0) s.erase(0, i);
    return s;
}

std::wstring ToWideLoose(const std::string& s) {
    return ToWide(s.c_str());
}

std::string NarrowAsciiLower(const std::wstring& ws) {
    std::string out;
    out.reserve(ws.size());
    for (wchar_t ch : ws) {
        if (ch >= 32 && ch <= 126) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else {
            out.push_back(' ');
        }
    }
    return out;
}

std::string GetExeDirA() {
    char path[MAX_PATH]{};
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return std::string();
    char* slash = std::strrchr(path, '\\');
    if (slash) *slash = '\0';
    return std::string(path);
}

bool FileExistsA(const std::string& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

struct BundledFileEntry {
    std::string name;
    uint64_t offset = 0;
    uint64_t size = 0;
};

static uint64_t Fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= uint64_t(c);
        h *= 1099511628211ull;
    }
    return h;
}

static bool EnsureDirectoryExistsA(const std::string& dir) {
    if (dir.empty()) return false;
    if (FileExistsA(dir)) return false;
    DWORD attr = GetFileAttributesA(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;

    std::string path = dir;
    for (char& ch : path) {
        if (ch == '/') ch = '\\';
    }

    size_t start = 0;
    if (path.size() >= 2 && path[1] == ':') start = 3;
    for (size_t i = start; i < path.size(); ++i) {
        if (path[i] != '\\') continue;
        std::string part = path.substr(0, i);
        if (part.empty()) continue;
        CreateDirectoryA(part.c_str(), nullptr);
    }
    CreateDirectoryA(path.c_str(), nullptr);
    attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string GetSelfPathA() {
    char path[MAX_PATH]{};
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return std::string();
    return std::string(path);
}

std::string GetBundledExtractDirA() {
    char tempPath[MAX_PATH]{};
    DWORD len = GetTempPathA(MAX_PATH, tempPath);
    std::string base = (len > 0 && len < MAX_PATH) ? std::string(tempPath) : std::string(".\\");
    while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
    const std::string selfPath = GetSelfPathA();
    char hashBuf[32];
    std::snprintf(hashBuf, sizeof(hashBuf), "%016llx", static_cast<unsigned long long>(Fnv1a64(selfPath)));
    return base + "\\huilang_adb_bundle\\" + hashBuf;
}

static bool ReadBundledManifest(std::vector<BundledFileEntry>& entries, std::string* err) {
    entries.clear();
    const std::string selfPath = GetSelfPathA();
    if (selfPath.empty()) {
        if (err) *err = "self path unavailable";
        return false;
    }

    FILE* fp = std::fopen(selfPath.c_str(), "rb");
    if (!fp) {
        if (err) *err = "open self failed";
        return false;
    }

    static const unsigned char kMagic[8] = {'H','L','A','D','B','P','K','1'};
    bool ok = false;

    do {
        if (_fseeki64(fp, 0, SEEK_END) != 0) {
            if (err) *err = "seek end failed";
            break;
        }
        const int64_t fileSize = _ftelli64(fp);
        if (fileSize < 24) {
            if (err) *err = "no bundled payload";
            break;
        }

        if (_fseeki64(fp, fileSize - 24, SEEK_SET) != 0) {
            if (err) *err = "seek trailer failed";
            break;
        }

        unsigned char trailer[24]{};
        if (std::fread(trailer, 1, sizeof(trailer), fp) != sizeof(trailer)) {
            if (err) *err = "read trailer failed";
            break;
        }
        if (std::memcmp(trailer, kMagic, 8) != 0) {
            if (err) *err = "bundle magic missing";
            break;
        }

        uint64_t manifestOffset = 0;
        uint64_t manifestSize = 0;
        std::memcpy(&manifestOffset, trailer + 8, sizeof(uint64_t));
        std::memcpy(&manifestSize, trailer + 16, sizeof(uint64_t));

        if (manifestOffset >= static_cast<uint64_t>(fileSize) ||
            manifestSize > static_cast<uint64_t>(fileSize) ||
            manifestOffset + manifestSize > static_cast<uint64_t>(fileSize - 24)) {
            if (err) *err = "bundle manifest range invalid";
            break;
        }

        if (_fseeki64(fp, static_cast<int64_t>(manifestOffset), SEEK_SET) != 0) {
            if (err) *err = "seek manifest failed";
            break;
        }

        std::vector<unsigned char> manifest(static_cast<size_t>(manifestSize));
        if (!manifest.empty() && std::fread(manifest.data(), 1, manifest.size(), fp) != manifest.size()) {
            if (err) *err = "read manifest failed";
            break;
        }

        size_t pos = 0;
        auto readU16 = [&](uint16_t& out) -> bool {
            if (pos + 2 > manifest.size()) return false;
            std::memcpy(&out, manifest.data() + pos, 2);
            pos += 2;
            return true;
        };
        auto readU32 = [&](uint32_t& out) -> bool {
            if (pos + 4 > manifest.size()) return false;
            std::memcpy(&out, manifest.data() + pos, 4);
            pos += 4;
            return true;
        };
        auto readU64 = [&](uint64_t& out) -> bool {
            if (pos + 8 > manifest.size()) return false;
            std::memcpy(&out, manifest.data() + pos, 8);
            pos += 8;
            return true;
        };

        uint32_t version = 0;
        uint32_t count = 0;
        if (!readU32(version) || !readU32(count) || version != 1) {
            if (err) *err = "bundle manifest version invalid";
            break;
        }

        for (uint32_t i = 0; i < count; ++i) {
            uint16_t nameLen = 0;
            uint64_t offset = 0;
            uint64_t size = 0;
            if (!readU16(nameLen) || pos + nameLen > manifest.size()) {
                if (err) *err = "bundle manifest name invalid";
                entries.clear();
                break;
            }
            std::string name(reinterpret_cast<const char*>(manifest.data() + pos), nameLen);
            pos += nameLen;
            if (!readU64(offset) || !readU64(size)) {
                if (err) *err = "bundle manifest entry truncated";
                entries.clear();
                break;
            }
            if (offset + size > manifestOffset) {
                if (err) *err = "bundle payload range invalid";
                entries.clear();
                break;
            }
            entries.push_back({name, offset, size});
        }
        if (entries.empty()) {
            if (count == 0) {
                if (err) *err = "bundle empty";
            }
            break;
        }
        ok = true;
    } while (false);

    std::fclose(fp);
    return ok;
}

static bool ExtractBundledAdbFiles(const std::string& outDir, std::string* err) {
    std::vector<BundledFileEntry> entries;
    if (!ReadBundledManifest(entries, err)) return false;
    if (!EnsureDirectoryExistsA(outDir)) {
        if (err) *err = "create temp dir failed";
        return false;
    }

    const std::string selfPath = GetSelfPathA();
    FILE* fp = std::fopen(selfPath.c_str(), "rb");
    if (!fp) {
        if (err) *err = "open self failed";
        return false;
    }

    bool ok = true;
    std::vector<unsigned char> buffer(1024 * 1024);

    for (const auto& entry : entries) {
        const std::string target = outDir + "\\" + entry.name;
        bool needWrite = true;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExA(target.c_str(), GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER sz{};
            sz.LowPart = fad.nFileSizeLow;
            sz.HighPart = fad.nFileSizeHigh;
            if (sz.QuadPart == entry.size) {
                needWrite = false;
            }
        }
        if (!needWrite) continue;

        if (_fseeki64(fp, static_cast<int64_t>(entry.offset), SEEK_SET) != 0) {
            ok = false;
            if (err) *err = "seek payload failed";
            break;
        }

        FILE* out = std::fopen(target.c_str(), "wb");
        if (!out) {
            ok = false;
            if (err) *err = "write bundled file failed";
            break;
        }

        uint64_t remaining = entry.size;
        while (remaining > 0) {
            const size_t chunk = static_cast<size_t>((std::min<uint64_t>)(remaining, buffer.size()));
            if (std::fread(buffer.data(), 1, chunk, fp) != chunk) {
                ok = false;
                if (err) *err = "read payload failed";
                break;
            }
            if (std::fwrite(buffer.data(), 1, chunk, out) != chunk) {
                ok = false;
                if (err) *err = "write payload failed";
                break;
            }
            remaining -= chunk;
        }
        std::fclose(out);
        if (!ok) break;
    }

    std::fclose(fp);
    return ok;
}

std::string ResolveAdbPath(std::string* resolveNote) {
    const std::string exeDir = GetExeDirA();
    if (!exeDir.empty()) {
        const std::string localAdb = exeDir + "\\adb.exe";
        if (FileExistsA(localAdb)) {
            if (resolveNote) *resolveNote = "local";
            return localAdb;
        }
    }

    const std::string outDir = GetBundledExtractDirA();
    const std::string bundledAdb = outDir + "\\adb.exe";
    std::string extractErr;
    if (FileExistsA(bundledAdb) || ExtractBundledAdbFiles(outDir, &extractErr)) {
        if (FileExistsA(bundledAdb)) {
            if (resolveNote) *resolveNote = "bundled";
            return bundledAdb;
        }
    }

    if (resolveNote) {
        *resolveNote = extractErr.empty() ? "path" : extractErr;
    }
    return "adb";
}

std::wstring ToWide(const char* s) {
    if (!s) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring out(needed - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), needed);
    return out;
}

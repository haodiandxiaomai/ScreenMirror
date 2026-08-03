#pragma once

#include <string>

struct CommandResult {
    int rc = -1;
    std::string output;
};
int64_t NowNs();
double DiffMs(int64_t endNs, int64_t beginNs);
CommandResult RunCommandCapture(const std::string& cmd);
std::string TrimAscii(std::string s);
std::wstring ToWide(const char* s);
std::wstring ToWideLoose(const std::string& s);
std::string NarrowAsciiLower(const std::wstring& ws);
std::string GetExeDirA();
bool FileExistsA(const std::string& path);
std::string GetSelfPathA();
std::string GetBundledExtractDirA();
std::string ResolveAdbPath(std::string* resolveNote = nullptr);

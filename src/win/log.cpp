#include "win/log.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace sp::win {

namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
}  // namespace

void log_open(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) return;
    g_file = _wfopen(path.c_str(), L"a");
}

void logf(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_file) return;
    std::fprintf(g_file, "%02d:%02d:%02d.%03d  %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    std::fflush(g_file);
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace sp::win

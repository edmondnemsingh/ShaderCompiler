#pragma once

#include <string>

namespace sp::win {

// Appends timestamped lines to a log file (thread-safe). No-op until log_open() succeeds.
void log_open(const std::wstring& path);
void logf(const char* fmt, ...)
#if defined(__MINGW32__)
    __attribute__((format(gnu_printf, 1, 2)))
#elif defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);

}  // namespace sp::win

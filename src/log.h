// Minimal thread-safe logger. Writes timestamped, levelled lines to stderr.
#pragma once

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace jusiai {

enum class LogLevel { Debug = 0, Info, Warn, Error };

namespace detail {

inline std::mutex& log_mutex() {
  static std::mutex m;
  return m;
}

inline LogLevel& min_level() {
  static LogLevel level = LogLevel::Info;
  return level;
}

inline const char* level_tag(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
  }
  return "?????";
}

template <typename... Args>
void log_line(LogLevel level, const char* fmt, Args... args) {
  if (level < min_level()) return;

  std::time_t t = std::time(nullptr);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  char ts[20];
  std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

  std::lock_guard<std::mutex> lock(log_mutex());
  std::fprintf(stderr, "[%s] %s | ", ts, level_tag(level));
  // With no varargs, print the message verbatim — feeding a non-literal format
  // string to fprintf with zero arguments trips -Werror=format-security
  // (enabled by default on Ubuntu) and would also mis-parse a stray '%'.
  if constexpr (sizeof...(args) == 0) {
    std::fputs(fmt, stderr);
  } else {
    std::fprintf(stderr, fmt, args...);
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

}  // namespace detail

inline void set_log_level(LogLevel level) { detail::min_level() = level; }

}  // namespace jusiai

// printf-style logging macros.
#define LOG_DEBUG(...) ::jusiai::detail::log_line(::jusiai::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)  ::jusiai::detail::log_line(::jusiai::LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(...)  ::jusiai::detail::log_line(::jusiai::LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) ::jusiai::detail::log_line(::jusiai::LogLevel::Error, __VA_ARGS__)

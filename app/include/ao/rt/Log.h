// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/async/AsyncExceptionHandler.h>

#include <spdlog/common.h>
#include <spdlog/logger.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <source_location>

namespace ao::rt
{
  enum class LogLevel : std::uint8_t
  {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
    Off = 6
  };

  enum class LogConsoleMode : std::uint8_t
  {
    Enabled,
    Disabled
  };

  class Log final
  {
  public:
    // Logging threads must be stopped before shutdown; initialize/shutdown do not synchronize with log calls.
    static void initialize(LogLevel level = LogLevel::Info,
                           std::filesystem::path logDir = {},
                           LogConsoleMode consoleMode = LogConsoleMode::Enabled);
    static void shutdown();

    static async::AsyncExceptionHandler asyncExceptionHandler();

    static std::shared_ptr<spdlog::logger> const& appLogger() noexcept { return _appLoggerPtr; }
    static std::shared_ptr<spdlog::logger> const& audioLogger() noexcept { return _audioLoggerPtr; }

  private:
    static std::shared_ptr<spdlog::logger> _appLoggerPtr;
    static std::shared_ptr<spdlog::logger> _audioLoggerPtr;
    static bool _initialized;
    static std::mutex _lifecycleMutex;
  };

  /**
   * @brief Internal helper to convert C++20 source_location to spdlog source_loc.
   */
  inline spdlog::source_loc toSpdlog(std::source_location const& loc)
  {
    return {loc.file_name(), static_cast<std::int32_t>(loc.line()), loc.function_name()};
  }
} // namespace ao::rt

// Core log macros
#define AO_LOG_CALL(loggerExpr, level, loc, ...)                                                                       \
  do /* NOLINT(cppcoreguidelines-avoid-do-while) */                                                                    \
  {                                                                                                                    \
    auto const& loggerPtr = (loggerExpr);                                                                              \
    if (loggerPtr != nullptr && loggerPtr->should_log(level))                                                          \
    {                                                                                                                  \
      loggerPtr->log(ao::rt::toSpdlog(loc), level, __VA_ARGS__);                                                       \
    }                                                                                                                  \
  }                                                                                                                    \
  while (false)

#define APP_LOG_TRACE(...)                                                                                             \
  AO_LOG_CALL(ao::rt::Log::appLogger(), spdlog::level::trace, std::source_location::current(), __VA_ARGS__)
#define APP_LOG_DEBUG(...)                                                                                             \
  AO_LOG_CALL(ao::rt::Log::appLogger(), spdlog::level::debug, std::source_location::current(), __VA_ARGS__)
#define APP_LOG_INFO(...)                                                                                              \
  AO_LOG_CALL(ao::rt::Log::appLogger(), spdlog::level::info, std::source_location::current(), __VA_ARGS__)
#define APP_LOG_WARN(...)                                                                                              \
  AO_LOG_CALL(ao::rt::Log::appLogger(), spdlog::level::warn, std::source_location::current(), __VA_ARGS__)
#define APP_LOG_ERROR(...)                                                                                             \
  AO_LOG_CALL(ao::rt::Log::appLogger(), spdlog::level::err, std::source_location::current(), __VA_ARGS__)
#define APP_LOG_CRITICAL(...)                                                                                          \
  AO_LOG_CALL(ao::rt::Log::appLogger(), spdlog::level::critical, std::source_location::current(), __VA_ARGS__)

#define AUDIO_LOG_TRACE(...)                                                                                           \
  AO_LOG_CALL(ao::rt::Log::audioLogger(), spdlog::level::trace, std::source_location::current(), __VA_ARGS__)
#define AUDIO_LOG_DEBUG(...)                                                                                           \
  AO_LOG_CALL(ao::rt::Log::audioLogger(), spdlog::level::debug, std::source_location::current(), __VA_ARGS__)
#define AUDIO_LOG_INFO(...)                                                                                            \
  AO_LOG_CALL(ao::rt::Log::audioLogger(), spdlog::level::info, std::source_location::current(), __VA_ARGS__)
#define AUDIO_LOG_WARN(...)                                                                                            \
  AO_LOG_CALL(ao::rt::Log::audioLogger(), spdlog::level::warn, std::source_location::current(), __VA_ARGS__)
#define AUDIO_LOG_ERROR(...)                                                                                           \
  AO_LOG_CALL(ao::rt::Log::audioLogger(), spdlog::level::err, std::source_location::current(), __VA_ARGS__)
#define AUDIO_LOG_CRITICAL(...)                                                                                        \
  AO_LOG_CALL(ao::rt::Log::audioLogger(), spdlog::level::critical, std::source_location::current(), __VA_ARGS__)

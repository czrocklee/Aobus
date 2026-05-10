// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <spdlog/fmt/ostr.h> // for logging custom types
#include <spdlog/spdlog.h>

#include <filesystem>
#include <memory>
#include <source_location>
#include <string>

namespace ao::log
{
  enum class LogLevel
  {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
    Off = 6
  };

  class Log final
  {
  public:
    static void init(LogLevel level = LogLevel::Info, std::filesystem::path logDir = {});
    static void shutdown();

    static std::shared_ptr<spdlog::logger>& getAppLogger() { return _appLogger; }
    static std::shared_ptr<spdlog::logger>& getAudioLogger() { return _audioLogger; }

  private:
    static std::shared_ptr<spdlog::logger> _appLogger;
    static std::shared_ptr<spdlog::logger> _audioLogger;
  };

  /**
   * @brief Internal helper to convert C++20 source_location to spdlog source_loc.
   */
  inline spdlog::source_loc toSpdlog(std::source_location const& loc)
  {
    return {loc.file_name(), static_cast<int>(loc.line()), loc.function_name()};
  }
} // namespace ao::log

// Core log macros
#define AO_LOG_CALL(logger, level, loc, ...)                                                                           \
  if (logger->should_log(level)) logger->log(ao::log::toSpdlog(loc), level, __VA_ARGS__)

#define APP_LOG_TRACE(...)                                                                                             \
  AO_LOG_CALL(ao::log::Log::getAppLogger(), spdlog::level::trace, std::source_location::current(), __VA_ARGS__)
#define APP_LOG_DEBUG(...)                                                                                             \
  AO_LOG_CALL(ao::log::Log::getAppLogger(), spdlog::level::debug, std::source_location::current(), __VA_ARGS__)
#define APP_LOG_INFO(...)                                                                                              \
  AO_LOG_CALL(ao::log::Log::getAppLogger(), spdlog::level::info, std::source_location::current(), __VA_ARGS__)
#define APP_LOG_WARN(...)                                                                                              \
  AO_LOG_CALL(ao::log::Log::getAppLogger(), spdlog::level::warn, std::source_location::current(), __VA_ARGS__)
#define APP_LOG_ERROR(...)                                                                                             \
  AO_LOG_CALL(ao::log::Log::getAppLogger(), spdlog::level::err, std::source_location::current(), __VA_ARGS__)
#define APP_LOG_CRITICAL(...)                                                                                          \
  AO_LOG_CALL(ao::log::Log::getAppLogger(), spdlog::level::critical, std::source_location::current(), __VA_ARGS__)

#define AUDIO_LOG_TRACE(...)                                                                                           \
  AO_LOG_CALL(ao::log::Log::getAudioLogger(), spdlog::level::trace, std::source_location::current(), __VA_ARGS__)
#define AUDIO_LOG_DEBUG(...)                                                                                           \
  AO_LOG_CALL(ao::log::Log::getAudioLogger(), spdlog::level::debug, std::source_location::current(), __VA_ARGS__)
#define AUDIO_LOG_INFO(...)                                                                                            \
  AO_LOG_CALL(ao::log::Log::getAudioLogger(), spdlog::level::info, std::source_location::current(), __VA_ARGS__)
#define AUDIO_LOG_WARN(...)                                                                                            \
  AO_LOG_CALL(ao::log::Log::getAudioLogger(), spdlog::level::warn, std::source_location::current(), __VA_ARGS__)
#define AUDIO_LOG_ERROR(...)                                                                                           \
  AO_LOG_CALL(ao::log::Log::getAudioLogger(), spdlog::level::err, std::source_location::current(), __VA_ARGS__)
#define AUDIO_LOG_CRITICAL(...)                                                                                        \
  AO_LOG_CALL(ao::log::Log::getAudioLogger(), spdlog::level::critical, std::source_location::current(), __VA_ARGS__)

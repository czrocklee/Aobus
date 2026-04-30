// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <spdlog/fmt/ostr.h> // for logging custom types
#include <spdlog/spdlog.h>

#include <filesystem>
#include <memory>
#include <string>

namespace rs::log
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
    static std::shared_ptr<spdlog::logger>& getPlaybackLogger() { return _playbackLogger; }

  private:
    static std::shared_ptr<spdlog::logger> _appLogger;
    static std::shared_ptr<spdlog::logger> _playbackLogger;
  };
} // namespace rs::log

// Core log macros
#define APP_LOG_TRACE(...)                                                                                             \
  if (rs::log::Log::getAppLogger()->should_log(spdlog::level::trace)) rs::log::Log::getAppLogger()->trace(__VA_ARGS__)
#define APP_LOG_DEBUG(...)                                                                                             \
  if (rs::log::Log::getAppLogger()->should_log(spdlog::level::debug)) rs::log::Log::getAppLogger()->debug(__VA_ARGS__)
#define APP_LOG_INFO(...)                                                                                              \
  if (rs::log::Log::getAppLogger()->should_log(spdlog::level::info)) rs::log::Log::getAppLogger()->info(__VA_ARGS__)
#define APP_LOG_WARN(...)                                                                                              \
  if (rs::log::Log::getAppLogger()->should_log(spdlog::level::warn)) rs::log::Log::getAppLogger()->warn(__VA_ARGS__)
#define APP_LOG_ERROR(...)                                                                                             \
  if (rs::log::Log::getAppLogger()->should_log(spdlog::level::err)) rs::log::Log::getAppLogger()->error(__VA_ARGS__)
#define APP_LOG_CRITICAL(...)                                                                                          \
  if (rs::log::Log::getAppLogger()->should_log(spdlog::level::critical))                                               \
  rs::log::Log::getAppLogger()->critical(__VA_ARGS__)

// Playback log macros
#define PLAYBACK_LOG_TRACE(...)                                                                                        \
  if (rs::log::Log::getPlaybackLogger()->should_log(spdlog::level::trace))                                             \
  rs::log::Log::getPlaybackLogger()->trace(__VA_ARGS__)
#define PLAYBACK_LOG_DEBUG(...)                                                                                        \
  if (rs::log::Log::getPlaybackLogger()->should_log(spdlog::level::debug))                                             \
  rs::log::Log::getPlaybackLogger()->debug(__VA_ARGS__)
#define PLAYBACK_LOG_INFO(...)                                                                                         \
  if (rs::log::Log::getPlaybackLogger()->should_log(spdlog::level::info))                                              \
  rs::log::Log::getPlaybackLogger()->info(__VA_ARGS__)
#define PLAYBACK_LOG_WARN(...)                                                                                         \
  if (rs::log::Log::getPlaybackLogger()->should_log(spdlog::level::warn))                                              \
  rs::log::Log::getPlaybackLogger()->warn(__VA_ARGS__)
#define PLAYBACK_LOG_ERROR(...)                                                                                        \
  if (rs::log::Log::getPlaybackLogger()->should_log(spdlog::level::err))                                               \
  rs::log::Log::getPlaybackLogger()->error(__VA_ARGS__)
#define PLAYBACK_LOG_CRITICAL(...)                                                                                     \
  if (rs::log::Log::getPlaybackLogger()->should_log(spdlog::level::critical))                                          \
  rs::log::Log::getPlaybackLogger()->critical(__VA_ARGS__)

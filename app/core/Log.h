// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <spdlog/fmt/ostr.h> // for logging custom types
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace app::core
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
    static void init(LogLevel level = LogLevel::Info);
    static void shutdown();

    static std::shared_ptr<spdlog::logger>& getAppLogger() { return _appLogger; }
    static std::shared_ptr<spdlog::logger>& getPlaybackLogger() { return _playbackLogger; }

  private:
    static std::shared_ptr<spdlog::logger> _appLogger;
    static std::shared_ptr<spdlog::logger> _playbackLogger;
  };
} // namespace app::core

// Core log macros
#define APP_LOG_TRACE(...)                                                                                             \
  if (app::core::Log::getAppLogger()->should_log(spdlog::level::trace))                                                \
  app::core::Log::getAppLogger()->trace(__VA_ARGS__)
#define APP_LOG_DEBUG(...)                                                                                             \
  if (app::core::Log::getAppLogger()->should_log(spdlog::level::debug))                                                \
  app::core::Log::getAppLogger()->debug(__VA_ARGS__)
#define APP_LOG_INFO(...)                                                                                              \
  if (app::core::Log::getAppLogger()->should_log(spdlog::level::info)) app::core::Log::getAppLogger()->info(__VA_ARGS__)
#define APP_LOG_WARN(...)                                                                                              \
  if (app::core::Log::getAppLogger()->should_log(spdlog::level::warn)) app::core::Log::getAppLogger()->warn(__VA_ARGS__)
#define APP_LOG_ERROR(...)                                                                                             \
  if (app::core::Log::getAppLogger()->should_log(spdlog::level::err)) app::core::Log::getAppLogger()->error(__VA_ARGS__)
#define APP_LOG_CRITICAL(...)                                                                                          \
  if (app::core::Log::getAppLogger()->should_log(spdlog::level::critical))                                             \
  app::core::Log::getAppLogger()->critical(__VA_ARGS__)

// Playback log macros
#define PLAYBACK_LOG_TRACE(...)                                                                                        \
  if (app::core::Log::getPlaybackLogger()->should_log(spdlog::level::trace))                                           \
  app::core::Log::getPlaybackLogger()->trace(__VA_ARGS__)
#define PLAYBACK_LOG_DEBUG(...)                                                                                        \
  if (app::core::Log::getPlaybackLogger()->should_log(spdlog::level::debug))                                           \
  app::core::Log::getPlaybackLogger()->debug(__VA_ARGS__)
#define PLAYBACK_LOG_INFO(...)                                                                                         \
  if (app::core::Log::getPlaybackLogger()->should_log(spdlog::level::info))                                            \
  app::core::Log::getPlaybackLogger()->info(__VA_ARGS__)
#define PLAYBACK_LOG_WARN(...)                                                                                         \
  if (app::core::Log::getPlaybackLogger()->should_log(spdlog::level::warn))                                            \
  app::core::Log::getPlaybackLogger()->warn(__VA_ARGS__)
#define PLAYBACK_LOG_ERROR(...)                                                                                        \
  if (app::core::Log::getPlaybackLogger()->should_log(spdlog::level::err))                                             \
  app::core::Log::getPlaybackLogger()->error(__VA_ARGS__)
#define PLAYBACK_LOG_CRITICAL(...)                                                                                     \
  if (app::core::Log::getPlaybackLogger()->should_log(spdlog::level::critical))                                        \
  app::core::Log::getPlaybackLogger()->critical(__VA_ARGS__)

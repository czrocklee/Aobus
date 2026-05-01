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
    static std::shared_ptr<spdlog::logger>& getAudioLogger() { return _audioLogger; }

  private:
    static std::shared_ptr<spdlog::logger> _appLogger;
    static std::shared_ptr<spdlog::logger> _audioLogger;
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
#define AUDIO_LOG_TRACE(...)                                                                                        \
  if (rs::log::Log::getAudioLogger()->should_log(spdlog::level::trace))                                             \
  rs::log::Log::getAudioLogger()->trace(__VA_ARGS__)
#define AUDIO_LOG_DEBUG(...)                                                                                        \
  if (rs::log::Log::getAudioLogger()->should_log(spdlog::level::debug))                                             \
  rs::log::Log::getAudioLogger()->debug(__VA_ARGS__)
#define AUDIO_LOG_INFO(...)                                                                                         \
  if (rs::log::Log::getAudioLogger()->should_log(spdlog::level::info))                                              \
  rs::log::Log::getAudioLogger()->info(__VA_ARGS__)
#define AUDIO_LOG_WARN(...)                                                                                         \
  if (rs::log::Log::getAudioLogger()->should_log(spdlog::level::warn))                                              \
  rs::log::Log::getAudioLogger()->warn(__VA_ARGS__)
#define AUDIO_LOG_ERROR(...)                                                                                        \
  if (rs::log::Log::getAudioLogger()->should_log(spdlog::level::err))                                               \
  rs::log::Log::getAudioLogger()->error(__VA_ARGS__)
#define AUDIO_LOG_CRITICAL(...)                                                                                     \
  if (rs::log::Log::getAudioLogger()->should_log(spdlog::level::critical))                                          \
  rs::log::Log::getAudioLogger()->critical(__VA_ARGS__)

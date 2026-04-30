// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/utility/Log.h>

#include <rs/audio/AudioDecoderFactory.h>

#include <spdlog/async.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <vector>

namespace rs::log
{
  std::shared_ptr<spdlog::logger> Log::_appLogger = spdlog::null_logger_mt("app");
  std::shared_ptr<spdlog::logger> Log::_playbackLogger = spdlog::null_logger_mt("playback");

  void Log::init(LogLevel level, std::filesystem::path logDir)
  {
    if (logDir.empty())
    {
      logDir = std::filesystem::current_path() / "logs";
    }
    std::filesystem::create_directories(logDir);
    auto const logPath = logDir / "app.log";

    auto const spdLevel = static_cast<spdlog::level::level_enum>(level);

    // Setup sinks
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_pattern("%^[%T] %n: %v%$");
    consoleSink->set_level(spdLevel);

    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
      logPath.string(), 1024 * 1024 * 5, 3); // NOLINT(readability-magic-numbers)
    fileSink->set_pattern("[%Y-%m-%d %T.%e] [%l] %n: %v");
    fileSink->set_level(spdlog::level::trace);

    auto sinks = std::vector<spdlog::sink_ptr>{consoleSink, fileSink};

    // Initialize async registry
    spdlog::init_thread_pool(8192, 1);

    // Drop existing loggers to replace them with async versions
    spdlog::drop("app");
    spdlog::drop("playback");

    // Create loggers
    _appLogger = std::make_shared<spdlog::async_logger>(
      "app", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    _appLogger->set_level(spdlog::level::trace); // Keep internal level at trace, sinks will filter
    spdlog::register_logger(_appLogger);

    _playbackLogger = std::make_shared<spdlog::async_logger>(
      "playback", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    _playbackLogger->set_level(spdlog::level::trace);
    spdlog::register_logger(_playbackLogger);

    spdlog::set_default_logger(_appLogger);

    rs::audio::initializeAudioDecoders();

    APP_LOG_INFO("Logging initialized. Log file: {}", logPath.string());
  }

  void Log::shutdown()
  {
    APP_LOG_INFO("Shutting down logging...");
    spdlog::shutdown();
  }
} // namespace rs::log

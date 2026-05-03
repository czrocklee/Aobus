// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <ao/utility/Log.h>

#include <ao/audio/DecoderFactory.h>

#include <spdlog/async.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <vector>

namespace ao::log
{
  std::shared_ptr<spdlog::logger> Log::_appLogger = spdlog::null_logger_mt("app");
  std::shared_ptr<spdlog::logger> Log::_audioLogger = spdlog::null_logger_mt("audio");

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
    auto const consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_pattern("%^[%T] %n: %v%$");
    consoleSink->set_level(spdLevel);

    auto const fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
      logPath.string(), 1024 * 1024 * 5, 3); // NOLINT(readability-magic-numbers)
    fileSink->set_pattern("[%Y-%m-%d %T.%e] [%l] %n: %v");
    fileSink->set_level(spdlog::level::trace);

    auto const sinks = std::vector<spdlog::sink_ptr>{consoleSink, fileSink};

    // Initialize async registry
    spdlog::init_thread_pool(8192, 1);

    // Drop existing loggers to replace them with async versions
    spdlog::drop("app");
    spdlog::drop("audio");

    // Create loggers
    _appLogger = std::make_shared<spdlog::async_logger>(
      "app", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    _appLogger->set_level(spdlog::level::trace); // Keep internal level at trace, sinks will filter
    spdlog::register_logger(_appLogger);

    _audioLogger = std::make_shared<spdlog::async_logger>(
      "audio", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    _audioLogger->set_level(spdlog::level::trace);
    spdlog::register_logger(_audioLogger);

    spdlog::set_default_logger(_appLogger);

    ao::audio::initializeDecoders();

    APP_LOG_INFO("========================================================");
    APP_LOG_INFO("Logging initialized. Log file: {}", logPath.string());
  }

  void Log::shutdown()
  {
    APP_LOG_INFO("Shutting down logging...");

    if (_appLogger)
    {
      _appLogger->flush();
    }

    if (_audioLogger)
    {
      _audioLogger->flush();
    }

    spdlog::shutdown();
  }
} // namespace ao::log

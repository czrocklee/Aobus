// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ao/utility/Log.h"

#include "ao/audio/DecoderFactory.h"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ao::log
{
  namespace
  {
    constexpr std::size_t kAsyncQueueSize = 8192;
    constexpr std::size_t kAsyncThreadCount = 1;
    constexpr std::size_t kRotatingLogMaxSize = std::size_t{5} * 1024 * 1024;
    constexpr std::size_t kRotatingLogMaxFiles = 3;

    std::shared_ptr<spdlog::logger> makeNullLogger(std::string const& name)
    {
      auto logger = std::make_shared<spdlog::logger>(name, std::make_shared<spdlog::sinks::null_sink_mt>());
      logger->set_level(spdlog::level::off);
      return logger;
    }
  }

  std::shared_ptr<spdlog::logger> Log::_appLogger = makeNullLogger("app");
  std::shared_ptr<spdlog::logger> Log::_audioLogger = makeNullLogger("audio");
  bool Log::_initialized = false;
  std::mutex Log::_lifecycleMutex;

  void Log::init(LogLevel level, std::filesystem::path logDir)
  {
    auto const lock = std::scoped_lock{_lifecycleMutex};

    if (_initialized)
    {
      return;
    }

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
      logPath.string(), kRotatingLogMaxSize, kRotatingLogMaxFiles);
    fileSink->set_pattern("[%Y-%m-%d %T.%e] [%l] %n: %v");
    fileSink->set_level(spdlog::level::trace);

    auto const sinks = std::vector<spdlog::sink_ptr>{consoleSink, fileSink};

    // Initialize async registry
    spdlog::init_thread_pool(kAsyncQueueSize, kAsyncThreadCount);

    // Drop existing loggers to replace them with async versions
    spdlog::drop("app");
    spdlog::drop("audio");

    // Create loggers
    auto appLogger = std::make_shared<spdlog::async_logger>(
      "app", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    appLogger->set_level(spdlog::level::trace); // Keep internal level at trace, sinks will filter
    spdlog::register_logger(appLogger);

    auto audioLogger = std::make_shared<spdlog::async_logger>(
      "audio", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    audioLogger->set_level(spdlog::level::trace);
    spdlog::register_logger(audioLogger);

    _appLogger = appLogger;
    _audioLogger = audioLogger;

    spdlog::set_default_logger(appLogger);
    _initialized = true;

    audio::initializeDecoders();

    APP_LOG_INFO("========================================================");
    APP_LOG_INFO("Logging initialized. Log file: {}", logPath.string());
  }

  void Log::shutdown()
  {
    auto const lock = std::scoped_lock{_lifecycleMutex};

    if (!_initialized)
    {
      return;
    }

    APP_LOG_INFO("Shutting down logging...");

    auto const app = _appLogger;
    auto const audio = _audioLogger;

    if (app)
    {
      app->flush();
    }

    if (audio)
    {
      audio->flush();
    }

    _appLogger = makeNullLogger("app");
    _audioLogger = makeNullLogger("audio");
    _initialized = false;

    spdlog::shutdown();
  }

  bool Log::isInitialized()
  {
    auto const lock = std::scoped_lock{_lifecycleMutex};
    return _initialized;
  }
} // namespace ao::log

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Exception.h>
#include <ao/async/AsyncExceptionHandler.h>
#include <ao/rt/Log.h>

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    constexpr std::size_t kAsyncQueueSize = 8192;
    constexpr std::size_t kAsyncThreadCount = 1;
    constexpr std::size_t kRotatingLogMaxSize = std::size_t{5} * 1024 * 1024;
    constexpr std::size_t kRotatingLogMaxFiles = 3;

    std::shared_ptr<spdlog::logger> makeNullLogger(std::string const& name)
    {
      auto loggerPtr = std::make_shared<spdlog::logger>(name, std::make_shared<spdlog::sinks::null_sink_mt>());
      loggerPtr->set_level(spdlog::level::off);
      return loggerPtr;
    }
  } // namespace

  // Null loggers must exist before initialize().
  std::shared_ptr<spdlog::logger> Log::_appLoggerPtr = makeNullLogger("app");
  std::shared_ptr<spdlog::logger> Log::_audioLoggerPtr = makeNullLogger("audio");
  bool Log::_initialized = false;
  std::mutex Log::_lifecycleMutex;

  void Log::initialize(LogLevel level, std::filesystem::path logDir, LogConsoleMode consoleMode)
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

    auto const fileSinkPtr = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
      logPath.string(), kRotatingLogMaxSize, kRotatingLogMaxFiles);
    fileSinkPtr->set_pattern("[%Y-%m-%d %T.%e] [%l] %n: %v");
    fileSinkPtr->set_level(spdlog::level::trace);

    auto sinks = std::vector<spdlog::sink_ptr>{fileSinkPtr};

    if (consoleMode == LogConsoleMode::Enabled)
    {
      auto const consoleSinkPtr = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      consoleSinkPtr->set_pattern("%^[%T] %n: %v%$");
      consoleSinkPtr->set_level(spdLevel);
      sinks.push_back(consoleSinkPtr);
    }

    // Initialize async registry
    spdlog::init_thread_pool(kAsyncQueueSize, kAsyncThreadCount);

    // Drop existing loggers to replace them with async versions
    spdlog::drop("app");
    spdlog::drop("audio");

    // Create loggers
    auto appLoggerPtr = std::make_shared<spdlog::async_logger>(
      "app", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    appLoggerPtr->set_level(spdlog::level::trace);
    spdlog::register_logger(appLoggerPtr);

    auto audioLoggerPtr = std::make_shared<spdlog::async_logger>(
      "audio", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    audioLoggerPtr->set_level(spdlog::level::trace);
    spdlog::register_logger(audioLoggerPtr);

    _appLoggerPtr = appLoggerPtr;
    _audioLoggerPtr = audioLoggerPtr;

    spdlog::set_default_logger(appLoggerPtr);
    _initialized = true;

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

    auto const appLoggerPtr = _appLoggerPtr;
    auto const audioLoggerPtr = _audioLoggerPtr;

    if (appLoggerPtr)
    {
      appLoggerPtr->flush();
    }

    if (audioLoggerPtr)
    {
      audioLoggerPtr->flush();
    }

    _appLoggerPtr = makeNullLogger("app");
    _audioLoggerPtr = makeNullLogger("audio");
    _initialized = false;

    spdlog::shutdown();
  }

  bool Log::isInitialized()
  {
    auto const lock = std::scoped_lock{_lifecycleMutex};
    return _initialized;
  }

  async::AsyncExceptionHandler Log::asyncExceptionHandler()
  {
    auto const lock = std::scoped_lock{_lifecycleMutex};

    if (!_initialized)
    {
      return {};
    }

    auto loggerPtr = _appLoggerPtr;
    return [loggerPtr = std::move(loggerPtr)](std::exception_ptr exceptionPtr, std::string_view const context)
    {
      try
      {
        std::rethrow_exception(exceptionPtr);
      }
      catch (Exception const& exception)
      {
        loggerPtr->log(toSpdlog(exception.location()),
                       spdlog::level::critical,
                       "Unhandled exception in {}: {}",
                       context,
                       exception.what());
      }
      catch (std::exception const& exception)
      {
        loggerPtr->critical("Unhandled exception in {}: {}", context, exception.what());
      }
      catch (...)
      {
        loggerPtr->critical("Unhandled unknown exception in {}", context);
      }
    };
  }
} // namespace ao::rt

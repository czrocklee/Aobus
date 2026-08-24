// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/Log.h>

#include <ao/Contract.h>
#include <ao/compat/AtomicSharedPtr.h>
#include <ao/utility/Path.h>

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
  compat::AtomicSharedPtr<spdlog::logger> Log::_fatalLoggerPtr{makeNullLogger("fatal")};
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

    auto const logFileName = utility::pathToUtf8(logPath);
    auto const fileSinkPtr =
      std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logFileName, kRotatingLogMaxSize, kRotatingLogMaxFiles);
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
    spdlog::drop("fatal");

    // Create loggers
    auto appLoggerPtr = std::make_shared<spdlog::async_logger>(
      "app", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    appLoggerPtr->set_level(spdlog::level::trace);
    spdlog::register_logger(appLoggerPtr);

    auto audioLoggerPtr = std::make_shared<spdlog::async_logger>(
      "audio", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    audioLoggerPtr->set_level(spdlog::level::trace);
    spdlog::register_logger(audioLoggerPtr);

    auto fatalLoggerPtr = std::make_shared<spdlog::async_logger>(
      "fatal", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    fatalLoggerPtr->set_level(spdlog::level::critical);
    spdlog::register_logger(fatalLoggerPtr);

    _appLoggerPtr = appLoggerPtr;
    _audioLoggerPtr = audioLoggerPtr;
    _fatalLoggerPtr.store(fatalLoggerPtr, std::memory_order_release);

    spdlog::set_default_logger(appLoggerPtr);
    _initialized = true;

    auto const fatalSinkRegistered = registerFatalSink(&Log::submitFatal);
    AO_INVARIANT(fatalSinkRegistered, "Application fatal sink is already registered");

    APP_LOG_INFO("========================================================");
    APP_LOG_INFO("Logging initialized. Log file: {}", utility::pathToUtf8(logPath));
  }

  void Log::shutdown()
  {
    auto const lock = std::scoped_lock{_lifecycleMutex};

    if (!_initialized)
    {
      return;
    }

    APP_LOG_INFO("Shutting down logging...");

    auto const fatalSinkUnregistered = unregisterFatalSink(&Log::submitFatal);
    AO_INVARIANT(fatalSinkUnregistered, "Application fatal sink registration is not owned by Log");

    auto const appLoggerPtr = _appLoggerPtr;
    auto const audioLoggerPtr = _audioLoggerPtr;
    auto const fatalLoggerPtr = _fatalLoggerPtr.exchange(makeNullLogger("fatal"), std::memory_order_acq_rel);

    if (appLoggerPtr)
    {
      appLoggerPtr->flush();
    }

    if (audioLoggerPtr)
    {
      audioLoggerPtr->flush();
    }

    if (fatalLoggerPtr)
    {
      fatalLoggerPtr->flush();
    }

    _appLoggerPtr = makeNullLogger("app");
    _audioLoggerPtr = makeNullLogger("audio");
    _initialized = false;

    spdlog::shutdown();
  }

  bool Log::submitFatal(FatalDiagnostic const& diagnostic) noexcept
  {
    try
    {
      auto const loggerPtr = _fatalLoggerPtr.load(std::memory_order_acquire);

      if (!loggerPtr)
      {
        return false;
      }

      loggerPtr->log(toSpdlog(diagnostic.location),
                     spdlog::level::critical,
                     "AOBUS_FATAL category={} condition={} context={}{}",
                     fatalCategoryName(diagnostic.category),
                     diagnostic.condition,
                     diagnostic.context,
                     diagnostic.contextTruncated ? " [truncated]" : "");
      loggerPtr->flush();
      return true;
    }
    catch (...)
    {
      AO_AUDITED_CATCH(FatalSinkFallback);
      return false;
    }
  }
} // namespace ao::rt

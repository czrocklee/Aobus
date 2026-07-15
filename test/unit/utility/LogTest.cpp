// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/Log.h>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <source_location>
#include <stdexcept>
#include <string>

namespace ao::rt::test
{
  TEST_CASE("Log - initialization and shutdown", "[utility][unit][log]")
  {
    auto const tempDir = std::filesystem::temp_directory_path() / "ao_log_test";
    std::filesystem::remove_all(tempDir);

    // Log is a process-global singleton and Log::initialize() is a no-op while already
    // initialized. Other test cases init logging without shutting it down, so we
    // must clear any leaked state to guarantee init() actually targets tempDir.
    Log::shutdown();

    SECTION("Initialize with specific directory and log level")
    {
      Log::initialize(LogLevel::Debug, tempDir);

      auto const& appLoggerPtr = Log::appLogger();
      auto const& audioLoggerPtr = Log::audioLogger();

      CHECK(Log::isInitialized());
      CHECK(appLoggerPtr != nullptr);
      CHECK(audioLoggerPtr != nullptr);

      // Write a test log
      APP_LOG_DEBUG("Test app debug log");
      AUDIO_LOG_INFO("Test audio info log");

      {
        auto exceptionHandler = Log::asyncExceptionHandler();
        REQUIRE(exceptionHandler);
        exceptionHandler(std::make_exception_ptr(std::runtime_error{"async boom"}), "test coroutine");
      }

      Log::shutdown();
      CHECK_FALSE(Log::isInitialized());

      // Verify log file was created
      auto const logFile = tempDir / "app.log";
      CHECK(std::filesystem::exists(logFile));

      // Verify content (flush should have happened during shutdown)
      auto ifs = std::ifstream{logFile};
      auto content = std::string{(std::istreambuf_iterator{ifs}), std::istreambuf_iterator<char>{}};

      // We expect the initialized message and the test log to be present
      CHECK(content.contains("Logging initialized"));
      CHECK(content.contains("Test app debug log"));

      // Audio log writes to the same file sink, let's verify
      CHECK(content.contains("Test audio info log"));
      CHECK(content.contains("Unhandled exception in test coroutine: async boom"));
      CHECK(content.contains("Shutting down logging"));
    }

    SECTION("Initialize with empty directory (defaults to current_path/logs)")
    {
      auto const defaultDir = std::filesystem::current_path() / "logs";

      // Remove if exists to test creation
      if (std::filesystem::exists(defaultDir))
      {
        std::filesystem::remove_all(defaultDir);
      }

      Log::initialize(LogLevel::Warn, "");

      CHECK(Log::isInitialized());
      CHECK(std::filesystem::exists(defaultDir));
      CHECK(std::filesystem::exists(defaultDir / "app.log"));

      // Use toSpdlog directly to cover it
      auto const loc = std::source_location::current();
      auto const spdLoc = toSpdlog(loc);
      CHECK(spdLoc.filename != nullptr);

      Log::shutdown();
      CHECK_FALSE(Log::isInitialized());
    }

    std::filesystem::remove_all(tempDir);
  }
} // namespace ao::rt::test

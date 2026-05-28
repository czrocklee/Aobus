// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/utility/Log.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <source_location>
#include <string>

namespace ao::log::test
{
  TEST_CASE("Log - initialization and shutdown", "[utility][unit][log]")
  {
    auto const tempDir = std::filesystem::temp_directory_path() / "ao_log_test";
    std::filesystem::remove_all(tempDir);

    SECTION("Initialize with specific directory and log level")
    {
      Log::init(LogLevel::Debug, tempDir);

      auto const& appLogger = Log::appLogger();
      auto const& audioLogger = Log::audioLogger();

      REQUIRE(Log::isInitialized());
      REQUIRE(appLogger != nullptr);
      REQUIRE(audioLogger != nullptr);

      // Write a test log
      APP_LOG_DEBUG("Test app debug log");
      AUDIO_LOG_INFO("Test audio info log");

      Log::shutdown();
      CHECK_FALSE(Log::isInitialized());

      // Verify log file was created
      auto const logFile = tempDir / "app.log";
      REQUIRE(std::filesystem::exists(logFile));

      // Verify content (flush should have happened during shutdown)
      auto ifs = std::ifstream{logFile};
      auto content = std::string{(std::istreambuf_iterator{ifs}), std::istreambuf_iterator<char>{}};

      // We expect the initialized message and the test log to be present
      CHECK(content.find("Logging initialized") != std::string::npos);
      CHECK(content.find("Test app debug log") != std::string::npos);

      // Audio log writes to the same file sink, let's verify
      CHECK(content.find("Test audio info log") != std::string::npos);
      CHECK(content.find("Shutting down logging") != std::string::npos);
    }

    SECTION("Initialize with empty directory (defaults to current_path/logs)")
    {
      auto const defaultDir = std::filesystem::current_path() / "logs";

      // Remove if exists to test creation
      if (std::filesystem::exists(defaultDir))
      {
        std::filesystem::remove_all(defaultDir);
      }

      Log::init(LogLevel::Warn, "");

      REQUIRE(Log::isInitialized());
      REQUIRE(std::filesystem::exists(defaultDir));
      REQUIRE(std::filesystem::exists(defaultDir / "app.log"));

      // Use toSpdlog directly to cover it
      auto const loc = std::source_location::current();
      auto const spdLoc = toSpdlog(loc);
      CHECK(spdLoc.filename != nullptr);

      Log::shutdown();
      CHECK_FALSE(Log::isInitialized());
    }

    std::filesystem::remove_all(tempDir);
  }
} // namespace ao::log::test

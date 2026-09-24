// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/Log.h>

#include "test/unit/TestFixtureSupport.h"
#include <ao/Contract.h>

#include <catch2/catch_test_macros.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace ao::rt::test
{
  namespace
  {
    bool tryAcceptTestFatal(FatalDiagnostic const& /*diagnostic*/)
    {
      return true;
    }
  } // namespace

  TEST_CASE("Log - initialization and shutdown", "[runtime][unit][log]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& tempDir = temp.path();
    auto const originalDirectory = std::filesystem::current_path();
    auto restoreDirectory = gsl_lite::finally(
      [&originalDirectory]
      {
        auto error = std::error_code{};
        std::filesystem::current_path(originalDirectory, error);
      });
    auto shutdown = gsl_lite::finally([] { Log::shutdown(); });

    // Log is a process-global singleton and Log::initialize() is a no-op while already
    // initialized. Other test cases init logging without shutting it down, so we
    // must clear any leaked state to guarantee init() actually targets tempDir.
    Log::shutdown();

    SECTION("Initialize with specific directory")
    {
      Log::initialize(LogLevel::Debug, tempDir);

      auto const& appLoggerPtr = Log::appLogger();
      auto const& audioLoggerPtr = Log::audioLogger();

      CHECK(appLoggerPtr != nullptr);
      CHECK(audioLoggerPtr != nullptr);
      CHECK_FALSE(tryRegisterFatalSink(&tryAcceptTestFatal));

      // Write a test log
      APP_LOG_DEBUG("Test app debug log");
      AUDIO_LOG_INFO("Test audio info log");

      Log::shutdown();
      auto const registeredAfterShutdown = tryRegisterFatalSink(&tryAcceptTestFatal);
      auto unregister = gsl_lite::finally([] { std::ignore = tryUnregisterFatalSink(&tryAcceptTestFatal); });
      REQUIRE(registeredAfterShutdown);
      CHECK(tryUnregisterFatalSink(&tryAcceptTestFatal));

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
      CHECK(content.contains("Shutting down logging"));
    }

    SECTION("Initialize with empty directory (defaults to current_path/logs)")
    {
      std::filesystem::current_path(tempDir);
      auto const defaultDir = tempDir / "logs";

      Log::initialize(LogLevel::Warn, "");

      CHECK(std::filesystem::exists(defaultDir));
      CHECK(std::filesystem::exists(defaultDir / "app.log"));
      CHECK_FALSE(tryRegisterFatalSink(&tryAcceptTestFatal));

      Log::shutdown();
      auto const registeredAfterShutdown = tryRegisterFatalSink(&tryAcceptTestFatal);
      auto unregister = gsl_lite::finally([] { std::ignore = tryUnregisterFatalSink(&tryAcceptTestFatal); });
      REQUIRE(registeredAfterShutdown);
      CHECK(tryUnregisterFatalSink(&tryAcceptTestFatal));
    }
  }

  TEST_CASE("toSpdlog preserves source file line and function", "[runtime][unit][log]")
  {
    auto const loc = std::source_location::current();
    auto const spdLoc = toSpdlog(loc);

    REQUIRE(spdLoc.filename != nullptr);
    REQUIRE(spdLoc.funcname != nullptr);
    CHECK(std::string_view{spdLoc.filename} == loc.file_name());
    CHECK(std::cmp_equal(spdLoc.line, loc.line()));
    CHECK(std::string_view{spdLoc.funcname} == loc.function_name());
  }
} // namespace ao::rt::test

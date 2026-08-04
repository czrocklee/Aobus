// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/app/DestructiveLibraryRestart.h>

#include <ao/Error.h>
#include <ao/Exception.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace ao::winui::test
{
  namespace
  {
    struct EventLog final
    {
      std::vector<std::string_view> values;

      void add(std::string_view const value) { values.push_back(value); }
    };
  } // namespace

  TEST_CASE("DestructiveLibraryRestart - releases the active graph before a successful launch and exits",
            "[winui][unit][app]")
  {
    auto events = EventLog{};
    auto const outcome = executeDestructiveLibraryRestart({
      .releaseActiveGraph = [&events] { events.add("release"); },
      .launchSuccessor = [&events] -> Result<>
      {
        events.add("launch");
        return {};
      },
      .reportLaunchFailure = [&events](Error const&) noexcept { events.add("report"); },
      .exitProcess = [&events] noexcept { events.add("exit"); },
    });

    CHECK(outcome == DestructiveLibraryRestartOutcome::Launched);
    CHECK(events.values == std::vector<std::string_view>{"release", "launch", "exit"});
  }

  TEST_CASE("DestructiveLibraryRestart - reports launch failure, does not roll back, and still exits",
            "[winui][unit][app]")
  {
    auto events = EventLog{};
    auto reportedCode = Error::Code::Generic;
    auto const outcome = executeDestructiveLibraryRestart({
      .releaseActiveGraph = [&events] { events.add("release"); },
      .launchSuccessor = [&events] -> Result<>
      {
        events.add("launch");
        return makeError(Error::Code::IoError, "CreateProcessW failed");
      },
      .reportLaunchFailure =
        [&events, &reportedCode](Error const& error) noexcept
      {
        events.add("report");
        reportedCode = error.code;
      },
      .exitProcess = [&events] noexcept { events.add("exit"); },
    });

    CHECK(outcome == DestructiveLibraryRestartOutcome::LaunchFailed);
    CHECK(reportedCode == Error::Code::IoError);
    CHECK(events.values == std::vector<std::string_view>{"release", "launch", "report", "exit"});
  }

  TEST_CASE("DestructiveLibraryRestart - converts a throwing launcher to a reported failure and exits",
            "[winui][unit][app]")
  {
    auto events = EventLog{};
    auto reportedCode = Error::Code::Generic;
    auto const outcome = executeDestructiveLibraryRestart({
      .releaseActiveGraph = [&events] { events.add("release"); },
      .launchSuccessor = [&events] -> Result<>
      {
        events.add("launch");
        throwException<Exception>("native launcher failure");
      },
      .reportLaunchFailure =
        [&events, &reportedCode](Error const& error) noexcept
      {
        events.add("report");
        reportedCode = error.code;
      },
      .exitProcess = [&events] noexcept { events.add("exit"); },
    });

    CHECK(outcome == DestructiveLibraryRestartOutcome::LaunchFailed);
    CHECK(reportedCode == Error::Code::InitFailed);
    CHECK(events.values == std::vector<std::string_view>{"release", "launch", "report", "exit"});
  }

  TEST_CASE("DestructiveLibraryRestart - a throwing release still launches the successor", "[winui][unit][app]")
  {
    // The parent is exiting either way, so a half-released parent costs nothing
    // a user can observe. A successor that never starts costs them their app.
    auto events = EventLog{};
    auto const outcome = executeDestructiveLibraryRestart({
      .releaseActiveGraph =
        [&events]
      {
        events.add("release");
        throwException<Exception>("native teardown failure");
      },
      .launchSuccessor = [&events] -> Result<>
      {
        events.add("launch");
        return {};
      },
      .reportLaunchFailure = [&events](Error const&) noexcept { events.add("report"); },
      .exitProcess = [&events] noexcept { events.add("exit"); },
    });

    CHECK(outcome == DestructiveLibraryRestartOutcome::Launched);
    CHECK(events.values == std::vector<std::string_view>{"release", "launch", "exit"});
  }

  TEST_CASE("DestructiveLibraryRestart - a missing required operation exits without releasing anything",
            "[winui][unit][app]")
  {
    auto events = EventLog{};
    auto reportedCode = Error::Code::Generic;
    auto reportedMessage = std::string{};
    auto const outcome = executeDestructiveLibraryRestart({
      .releaseActiveGraph = [&events] { events.add("release"); },
      .launchSuccessor = {},
      .reportLaunchFailure =
        [&events, &reportedCode, &reportedMessage](Error const& error) noexcept
      {
        events.add("report");
        reportedCode = error.code;
        reportedMessage = error.message;
      },
      .exitProcess = [&events] noexcept { events.add("exit"); },
    });

    CHECK(outcome == DestructiveLibraryRestartOutcome::LaunchFailed);
    CHECK(reportedCode == Error::Code::InvalidState);
    CHECK(reportedMessage.contains("launchSuccessor"));
    // Nothing is torn down when the caller cannot follow through on the restart.
    CHECK(events.values == std::vector<std::string_view>{"report", "exit"});
  }

  TEST_CASE("DestructiveLibraryRestart - tolerates absent optional operations", "[winui][unit][app]")
  {
    bool released = false;
    auto const outcome = executeDestructiveLibraryRestart({
      .releaseActiveGraph = [&released] { released = true; },
      .launchSuccessor = [] -> Result<> { return makeError(Error::Code::IoError, "no successor"); },
      .reportLaunchFailure = {},
      .exitProcess = {},
    });

    CHECK(outcome == DestructiveLibraryRestartOutcome::LaunchFailed);
    CHECK(released);
  }
} // namespace ao::winui::test

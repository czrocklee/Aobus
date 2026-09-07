// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/ExitController.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("ExitController - first request retires then posts exit", "[tui][unit][exit]")
  {
    auto events = std::vector<std::string>{};
    auto controller = ExitController{{
      .retire = [&events] { events.emplace_back("retire"); },
      .postExit = [&events] { events.emplace_back("postExit"); },
    }};

    controller.requestExit();

    CHECK(controller.phase() == ExitController::Phase::ExitPosted);
    CHECK(events == std::vector<std::string>{"retire", "postExit"});
  }

  TEST_CASE("ExitController - nested request during retire posts exit once", "[tui][unit][exit]")
  {
    auto events = std::vector<std::string>{};
    ExitController* owner = nullptr;
    auto controller = ExitController{{
      .retire =
        [&events, &owner]
      {
        events.emplace_back("retire");
        CHECK(owner->phase() == ExitController::Phase::ExitPosted);
        owner->requestExit();
      },
      .postExit = [&events] { events.emplace_back("postExit"); },
    }};
    owner = &controller;

    controller.requestExit();

    CHECK(controller.phase() == ExitController::Phase::ExitPosted);
    CHECK(events == std::vector<std::string>{"retire", "postExit"});
  }

  TEST_CASE("ExitController - later requests after posting are no-ops", "[tui][unit][exit]")
  {
    std::int32_t postCount = 0;
    std::int32_t retireCount = 0;
    auto controller = ExitController{{
      .retire = [&retireCount] { ++retireCount; },
      .postExit = [&postCount] { ++postCount; },
    }};

    controller.requestExit();
    controller.requestExit();
    controller.requestExit();

    CHECK(retireCount == 1);
    CHECK(postCount == 1);
  }

  TEST_CASE("ExitController - waits for a pending submitted write before posting exit", "[tui][unit][exit]")
  {
    auto events = std::vector<std::string>{};
    auto controller = ExitController{{
      .retire = [&events] { events.emplace_back("retire"); },
      .postExit = [&events] { events.emplace_back("postExit"); },
      .hasPendingSubmittedWrite = [] { return true; },
    }};

    controller.requestExit();

    CHECK(controller.phase() == ExitController::Phase::WaitingForSubmittedWrite);
    CHECK(controller.isWaitingForSubmittedWrite());
    CHECK(events == std::vector<std::string>{"retire"});

    controller.notifySubmittedWriteSettled();

    CHECK(controller.phase() == ExitController::Phase::ExitPosted);
    CHECK(events == std::vector<std::string>{"retire", "postExit"});

    // The write settles once; a repeated notification cannot post a second exit.
    controller.notifySubmittedWriteSettled();

    CHECK(events == std::vector<std::string>{"retire", "postExit"});
  }

  TEST_CASE("ExitController - a second request stops application-level waiting", "[tui][unit][exit]")
  {
    std::int32_t postCount = 0;
    std::int32_t retireCount = 0;
    auto controller = ExitController{{
      .retire = [&retireCount] { ++retireCount; },
      .postExit = [&postCount] { ++postCount; },
      .hasPendingSubmittedWrite = [] { return true; },
    }};

    controller.requestExit();

    CHECK(controller.phase() == ExitController::Phase::WaitingForSubmittedWrite);
    CHECK(postCount == 0);

    controller.requestExit();

    CHECK(controller.phase() == ExitController::Phase::ExitPosted);
    CHECK(retireCount == 1);
    CHECK(postCount == 1);

    // A settlement that arrives after the user stopped waiting changes nothing.
    controller.notifySubmittedWriteSettled();

    CHECK(postCount == 1);
  }

  TEST_CASE("ExitController - the pending question is asked once, before retirement", "[tui][unit][exit]")
  {
    auto events = std::vector<std::string>{};
    std::int32_t askCount = 0;
    auto controller = ExitController{{
      .retire = [&events] { events.emplace_back("retire"); },
      .postExit = [&events] { events.emplace_back("postExit"); },
      .hasPendingSubmittedWrite =
        [&askCount, &events]
      {
        ++askCount;
        events.emplace_back("ask");
        return false;
      },
    }};

    controller.requestExit();
    controller.requestExit();

    CHECK(askCount == 1);
    CHECK(events == std::vector<std::string>{"ask", "retire", "postExit"});
  }

  TEST_CASE("ExitController - a write settled during retirement posts exit once", "[tui][unit][exit]")
  {
    auto events = std::vector<std::string>{};
    ExitController* owner = nullptr;
    auto controller = ExitController{{
      .retire =
        [&events, &owner]
      {
        events.emplace_back("retire");
        CHECK(owner->phase() == ExitController::Phase::WaitingForSubmittedWrite);
        owner->notifySubmittedWriteSettled();
      },
      .postExit = [&events] { events.emplace_back("postExit"); },
      .hasPendingSubmittedWrite = [] { return true; },
    }};
    owner = &controller;

    controller.requestExit();

    CHECK(controller.phase() == ExitController::Phase::ExitPosted);
    CHECK(events == std::vector<std::string>{"retire", "postExit"});
  }

  TEST_CASE("ExitController - a settlement without an exit request changes nothing", "[tui][unit][exit]")
  {
    std::int32_t postCount = 0;
    auto controller = ExitController{{
      .retire = [] {},
      .postExit = [&postCount] { ++postCount; },
      .hasPendingSubmittedWrite = [] { return true; },
    }};

    controller.notifySubmittedWriteSettled();

    CHECK(controller.phase() == ExitController::Phase::Running);
    CHECK(postCount == 0);
  }
} // namespace ao::tui::test

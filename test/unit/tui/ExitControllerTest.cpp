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
} // namespace ao::tui::test

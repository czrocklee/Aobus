// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/LibraryWindowLifecycle.h"

#include <ao/Error.h>
#include <ao/Exception.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    struct CandidateSentinel final
    {
      explicit CandidateSentinel(std::int32_t& destructionCountRef)
        : destructionCount{destructionCountRef}
      {
      }

      ~CandidateSentinel() { ++destructionCount; }

      CandidateSentinel(CandidateSentinel const&) = delete;
      CandidateSentinel& operator=(CandidateSentinel const&) = delete;
      CandidateSentinel(CandidateSentinel&&) = delete;
      CandidateSentinel& operator=(CandidateSentinel&&) = delete;

      std::int32_t& destructionCount;
    };
  } // namespace

  TEST_CASE("LibraryWindowLifecycle - same root reuses active pair without preparation or persistence",
            "[gtk][regression][active-library]")
  {
    auto events = std::vector<std::string>{};
    auto const unexpected = [&] { FAIL("replacement callback must not run for the active root"); };

    auto const opened = openLibraryWindow("/music/current",
                                          "/music/current",
                                          true,
                                          LibraryWindowReplacementCallbacks{
                                            .prepareCandidate = unexpected,
                                            .configureCandidate = unexpected,
                                            .retireActive = [&] -> Result<>
                                            {
                                              unexpected();
                                              return {};
                                            },
                                            .activateCandidate = unexpected,
                                            .replaceActiveSlot = unexpected,
                                            .releaseRetired = unexpected,
                                            .persistSelectedPath = unexpected,
                                            .scanActive = [&] { events.emplace_back("scan"); },
                                            .presentActive = [&] { events.emplace_back("present"); },
                                          });

    REQUIRE(opened);
    CHECK(*opened == LibraryWindowOpenOutcome::Reused);
    CHECK(events == std::vector<std::string>{"scan", "present"});
  }

  TEST_CASE("LibraryWindowLifecycle - candidate preparation exceptions leave the active pair unchanged",
            "[gtk][regression][active-library]")
  {
    auto activeSlot = std::string{"old"};
    auto applicationWindows = std::vector<std::string>{"old"};
    bool activeVisible = true;
    auto selectedPath = std::string{"/music/old"};
    std::int32_t activePlaybackTrack = 42;
    std::int32_t candidateDestructionCount = 0;

    auto const open = [&]
    {
      auto candidatePtr = std::unique_ptr<CandidateSentinel>{};
      std::ignore = openLibraryWindow("/music/old",
                                      "/music/new",
                                      false,
                                      LibraryWindowReplacementCallbacks{
                                        .prepareCandidate =
                                          [&]
                                        {
                                          candidatePtr = std::make_unique<CandidateSentinel>(candidateDestructionCount);
                                          throwException<Exception>("database open failed");
                                        },
                                        .configureCandidate = [&] { applicationWindows.emplace_back("configured"); },
                                        .retireActive = [&] -> Result<>
                                        {
                                          activePlaybackTrack = 0;
                                          return {};
                                        },
                                        .activateCandidate = [&] { applicationWindows.emplace_back("new"); },
                                        .replaceActiveSlot = [&] { activeSlot = "new"; },
                                        .releaseRetired = [&] { activeVisible = false; },
                                        .persistSelectedPath = [&] { selectedPath = "/music/new"; },
                                        .scanActive = [] {},
                                        .presentActive = [] {},
                                      });
    };

    CHECK_THROWS_AS(open(), Exception);
    CHECK(activeSlot == "old");
    CHECK(applicationWindows == std::vector<std::string>{"old"});
    CHECK(activeVisible);
    CHECK(selectedPath == "/music/old");
    CHECK(activePlaybackTrack == 42);
    CHECK(candidateDestructionCount == 1);
  }

  TEST_CASE("LibraryWindowLifecycle - candidate configuration exceptions destroy only the candidate",
            "[gtk][regression][active-library]")
  {
    auto activeSlot = std::string{"old"};
    auto applicationWindows = std::vector<std::string>{"old"};
    bool activeVisible = true;
    auto selectedPath = std::string{"/music/old"};
    std::int32_t activePlaybackTrack = 42;
    std::int32_t candidateDestructionCount = 0;

    auto const open = [&]
    {
      auto candidatePtr = std::unique_ptr<CandidateSentinel>{};
      std::ignore = openLibraryWindow(
        "/music/old",
        "/music/new",
        false,
        LibraryWindowReplacementCallbacks{
          .prepareCandidate = [&] { candidatePtr = std::make_unique<CandidateSentinel>(candidateDestructionCount); },
          .configureCandidate = [] { throwException<Exception>("callback installation failed"); },
          .retireActive = [&] -> Result<>
          {
            activePlaybackTrack = 0;
            return {};
          },
          .activateCandidate = [&] { applicationWindows.emplace_back("new"); },
          .replaceActiveSlot = [&] { activeSlot = "new"; },
          .releaseRetired = [&] { activeVisible = false; },
          .persistSelectedPath = [&] { selectedPath = "/music/new"; },
          .scanActive = [] {},
          .presentActive = [] {},
        });
    };

    CHECK_THROWS_AS(open(), Exception);
    CHECK(activeSlot == "old");
    CHECK(applicationWindows == std::vector<std::string>{"old"});
    CHECK(activeVisible);
    CHECK(selectedPath == "/music/old");
    CHECK(activePlaybackTrack == 42);
    CHECK(candidateDestructionCount == 1);
  }

  TEST_CASE("LibraryWindowLifecycle - discard failure follows complete candidate preparation",
            "[gtk][regression][active-library]")
  {
    auto events = std::vector<std::string>{};
    auto activeSlot = std::string{"old"};
    std::int32_t candidateDestructionCount = 0;

    {
      auto candidatePtr = std::unique_ptr<CandidateSentinel>{};
      auto const opened = openLibraryWindow("/music/old",
                                            "/music/new",
                                            true,
                                            LibraryWindowReplacementCallbacks{
                                              .prepareCandidate =
                                                [&]
                                              {
                                                events.emplace_back("prepare");
                                                candidatePtr =
                                                  std::make_unique<CandidateSentinel>(candidateDestructionCount);
                                              },
                                              .configureCandidate = [&] { events.emplace_back("configure"); },
                                              .retireActive = [&] -> Result<>
                                              {
                                                events.emplace_back("discard");
                                                return makeError(Error::Code::IoError, "discard failed");
                                              },
                                              .activateCandidate = [&] { events.emplace_back("activate"); },
                                              .replaceActiveSlot = [&] { activeSlot = "new"; },
                                              .releaseRetired = [&] { events.emplace_back("release"); },
                                              .persistSelectedPath = [&] { events.emplace_back("persist"); },
                                              .scanActive = [&] { events.emplace_back("scan"); },
                                              .presentActive = [] {},
                                            });

      REQUIRE_FALSE(opened);
      CHECK(opened.error().code == Error::Code::IoError);
      CHECK(activeSlot == "old");
    }

    CHECK(candidateDestructionCount == 1);
    CHECK(events == std::vector<std::string>{"prepare", "configure", "discard"});
  }

  TEST_CASE("LibraryWindowLifecycle - successful replacement commits before persistence and scan",
            "[gtk][regression][active-library]")
  {
    auto events = std::vector<std::string>{};
    auto activeSlot = std::string{"old"};
    auto applicationWindows = std::vector<std::string>{"old"};
    bool oldReleased = false;
    bool candidatePlaybackIdle = false;

    auto const opened = openLibraryWindow("/music/old",
                                          "/music/new",
                                          true,
                                          LibraryWindowReplacementCallbacks{
                                            .prepareCandidate = [&] { events.emplace_back("prepare"); },
                                            .configureCandidate = [&] { events.emplace_back("configure"); },
                                            .retireActive = [&] -> Result<>
                                            {
                                              events.emplace_back("checkpoint-discard");
                                              return {};
                                            },
                                            .activateCandidate =
                                              [&]
                                            {
                                              events.emplace_back("activate-idle");
                                              candidatePlaybackIdle = true;
                                              applicationWindows.emplace_back("new");
                                            },
                                            .replaceActiveSlot =
                                              [&]
                                            {
                                              events.emplace_back("replace-slot");
                                              activeSlot = "new";
                                            },
                                            .releaseRetired =
                                              [&]
                                            {
                                              events.emplace_back("release-old");
                                              applicationWindows.erase(applicationWindows.begin());
                                              oldReleased = true;
                                            },
                                            .persistSelectedPath =
                                              [&]
                                            {
                                              events.emplace_back("persist");
                                              CHECK(activeSlot == "new");
                                              CHECK(oldReleased);
                                              CHECK(applicationWindows == std::vector<std::string>{"new"});
                                            },
                                            .scanActive = [&] { events.emplace_back("scan"); },
                                            .presentActive = [] {},
                                          });

    REQUIRE(opened);
    CHECK(*opened == LibraryWindowOpenOutcome::Replaced);
    CHECK(candidatePlaybackIdle);
    CHECK(activeSlot == "new");
    CHECK(applicationWindows == std::vector<std::string>{"new"});
    CHECK(events == std::vector<std::string>{"prepare",
                                             "configure",
                                             "checkpoint-discard",
                                             "activate-idle",
                                             "replace-slot",
                                             "release-old",
                                             "persist",
                                             "scan"});
  }

  TEST_CASE("LibraryWindowLifecycle - persistence failure keeps replacement active and starts scan",
            "[gtk][regression][active-library]")
  {
    auto activeSlot = std::string{"old"};
    bool scanned = false;

    auto const opened = openLibraryWindow("/music/old",
                                          "/music/new",
                                          true,
                                          LibraryWindowReplacementCallbacks{
                                            .prepareCandidate = [] {},
                                            .configureCandidate = [] {},
                                            .retireActive = [] { return Result<>{}; },
                                            .activateCandidate = [] {},
                                            .replaceActiveSlot = [&] { activeSlot = "new"; },
                                            .releaseRetired = [] {},
                                            .persistSelectedPath = [] { throwException<Exception>("disk full"); },
                                            .scanActive = [&] { scanned = true; },
                                            .presentActive = [] {},
                                          });

    REQUIRE(opened);
    CHECK(*opened == LibraryWindowOpenOutcome::Replaced);
    CHECK(activeSlot == "new");
    CHECK(scanned);
  }
} // namespace ao::gtk::test

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/TerminalTitle.h"

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "tui/TerminalTitleFormat.h"
#include "tui/TextCell.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/utility/UnicodeText.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("TerminalTitle - compiles existing format language and empty disables", "[tui][unit][terminal-title]")
  {
    auto emptyRes = compileTerminalTitleFormat("");
    REQUIRE(emptyRes);
    CHECK_FALSE(*emptyRes);
    CHECK(compileTerminalTitleFormat(R"($artist " - " $title " " %catalog)"));
    CHECK_FALSE(compileTerminalTitleFormat("$missing"));
    CHECK_FALSE(compileTerminalTitleFormat("$year > 2000"));
  }

  TEST_CASE("TerminalTitle - writes only changed playing titles and balances title ownership",
            "[tui][regression][terminal-title]")
  {
    auto temp = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(temp, std::make_unique<rt::test::QueuedExecutor>());
    auto const first = rt::test::addRuntimeTrack(*runtimePtr, {.title = "First", .artist = "Artist"});
    auto const second = rt::test::addRuntimeTrack(*runtimePtr, {.title = "Second", .artist = "Other"});
    auto writes = std::vector<std::string>{};
    {
      auto title = TerminalTitle{runtimePtr->library(),
                                 [&](std::string_view value)
                                 {
                                   writes.emplace_back(value);
                                   return true;
                                 }};
      title.update(first);
      CHECK(writes.empty());
      REQUIRE(title.setFormat(R"($artist " - " $title)"));
      title.update(first);
      REQUIRE(writes.size() == 1);
      CHECK(writes.back() == "\033[22;2t\033]2;Artist - First\033\\");
      title.update(first);
      CHECK(writes.size() == 1);
      title.update(second);
      REQUIRE(writes.size() == 2);
      CHECK(writes.back() == "\033]2;Other - Second\033\\");
      CHECK_FALSE(title.setFormat("$unknown"));
      title.update(second);
      CHECK(writes.size() == 2);
      rt::test::updateRuntimeTrack(*runtimePtr, second, [](library::test::TrackSpec& spec) { spec.title = "Edited"; });
      title.update(second);
      CHECK(writes.back() == "\033]2;Other - Edited\033\\");
      title.update(kInvalidTrackId);
      CHECK(writes.back() == "\033]2;Aobus\033\\");
      REQUIRE(title.setFormat(""));
      title.update(first);
      CHECK(writes.back() == "\033[23;2t");
      auto const count = writes.size();
      title.restore();
      title.update(first);
      CHECK(writes.size() == count);
      REQUIRE(title.setFormat(R"("Fixed")"));
      title.update(first);
      CHECK(writes.back() == "\033[22;2t\033]2;Fixed\033\\");
    }
    CHECK(writes.back() == "\033[23;2t");
  }

  TEST_CASE("TerminalTitle - Soul replaces fallback branding and changes independently of track text",
            "[tui][regression][terminal-title]")
  {
    auto temp = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(temp, std::make_unique<rt::test::QueuedExecutor>());
    auto const track = rt::test::addRuntimeTrack(*runtimePtr, {.title = "Song"});
    auto writes = std::vector<std::string>{};
    auto title = TerminalTitle{runtimePtr->library(),
                               [&](std::string_view value)
                               {
                                 writes.emplace_back(value);
                                 return true;
                               }};
    REQUIRE(title.setFormat("$title"));
    title.update(track, "⠀⠉⠳");
    CHECK(writes.back() == "\033[22;2t\033]2;⠀⠉⠳ · Song\033\\");
    title.update(track, "⠚⠉⠓");
    CHECK(writes.back() == "\033]2;⠚⠉⠓ · Song\033\\");
    auto const count = writes.size();
    title.update(track, "⠚⠉⠓");
    CHECK(writes.size() == count);
    title.update(kInvalidTrackId, "⠀⠂⠀");
    CHECK(writes.back() == "\033]2;⠀⠂⠀\033\\");
    title.update(kInvalidTrackId);
    CHECK(writes.back() == "\033]2;Aobus\033\\");
    title.update(track);
    CHECK(writes.back() == "\033]2;Song\033\\");
    REQUIRE(title.setFormat(R"("Aobus")"));
    title.update(track, "⠀⠂⠀");
    CHECK(writes.back() == "\033]2;⠀⠂⠀ · Aobus\033\\");
    REQUIRE(title.setFormat(""));
    title.update(track, "⠀⠂⠀");
    CHECK(writes.back() == "\033[23;2t");
  }

  TEST_CASE("TerminalTitle - metadata controls cannot escape the title payload", "[tui][regression][terminal-title]")
  {
    auto temp = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(temp, std::make_unique<rt::test::QueuedExecutor>());
    auto const track = rt::test::addRuntimeTrack(*runtimePtr, {.title = "曲名\n\033]2;injected\a\xc2\x9c"});
    auto writes = std::vector<std::string>{};
    auto title = TerminalTitle{runtimePtr->library(),
                               [&](std::string_view value)
                               {
                                 writes.emplace_back(value);
                                 return true;
                               }};
    REQUIRE(title.setFormat("$title"));
    title.update(track);
    REQUIRE(writes.size() == 1);
    CHECK(writes.front() == "\033[22;2t\033]2;曲名  ]2;injected  \033\\");
  }

  TEST_CASE("TerminalTitle - failed writes restore once and disable further output",
            "[tui][regression][terminal-title]")
  {
    auto temp = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(temp, std::make_unique<rt::test::QueuedExecutor>());
    auto writes = std::vector<std::string>{};
    bool failUpdate = true;
    bool failRestore = false;
    bool returnFailure = false;
    std::size_t expectedWrites = 0;

    {
      auto title = TerminalTitle{runtimePtr->library(),
                                 [&](std::string_view value)
                                 {
                                   writes.emplace_back(value);

                                   if (value == "\033[23;2t" ? failRestore : failUpdate)
                                   {
                                     if (returnFailure)
                                     {
                                       return false;
                                     }

                                     throw std::system_error{std::make_error_code(std::errc::io_error)};
                                   }

                                   return true;
                                 }};
      REQUIRE(title.setFormat("$title"));

      SECTION("initial push may have reached the terminal")
      {
        CHECK(writes.empty());
      }

      SECTION("later title update fails after a successful push")
      {
        failUpdate = false;
        title.update(kInvalidTrackId, "⠀⠂⠀");
        REQUIRE(writes.size() == 1);
        failUpdate = true;
      }

      SECTION("the recovery pop also fails")
      {
        failRestore = true;
      }

      SECTION("both update and restoration report failure without throwing")
      {
        returnFailure = true;
        failRestore = true;
      }

      expectedWrites = writes.size() + 2;
      CHECK_NOTHROW(title.update(kInvalidTrackId, "⠀⠀⡷"));
      REQUIRE(writes.size() == expectedWrites);
      CHECK(writes.back() == "\033[23;2t");
      CHECK_NOTHROW(title.update(kInvalidTrackId, "⠞⠀⠀"));
      REQUIRE(title.setFormat(""));
      title.update(kInvalidTrackId);
      REQUIRE(title.setFormat("$title"));
      title.update(kInvalidTrackId);
      title.restore();
      CHECK(writes.size() == expectedWrites);
    }

    CHECK(writes.size() == expectedWrites);
  }

  TEST_CASE("TerminalTitle - unexpected sink exceptions remain programming failures",
            "[tui][regression][terminal-title]")
  {
    auto temp = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(temp, std::make_unique<rt::test::QueuedExecutor>());
    auto title = TerminalTitle{runtimePtr->library(),
                               [](std::string_view value)
                               {
                                 if (value != "\033[23;2t")
                                 {
                                   throw std::logic_error{"invalid sink state"};
                                 }

                                 return true;
                               }};
    REQUIRE(title.setFormat("$title"));
    CHECK_THROWS_AS(title.update(kInvalidTrackId), std::logic_error);
  }

  TEST_CASE("TerminalTitle - failed restoration retires ownership without escaping teardown",
            "[tui][regression][terminal-title]")
  {
    auto temp = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(temp, std::make_unique<rt::test::QueuedExecutor>());
    std::int32_t restoreAttempts = 0;

    {
      auto title = TerminalTitle{runtimePtr->library(),
                                 [&](std::string_view value)
                                 {
                                   if (value == "\033[23;2t")
                                   {
                                     ++restoreAttempts;
                                     throw std::system_error{std::make_error_code(std::errc::io_error)};
                                   }

                                   return true;
                                 }};
      REQUIRE(title.setFormat("$title"));
      title.update(kInvalidTrackId);

      SECTION("explicit restoration is best effort and is not retried")
      {
        CHECK_NOTHROW(title.restore());
        CHECK(restoreAttempts == 1);
        CHECK_NOTHROW(title.restore());
      }

      SECTION("destruction restores directly")
      {
        CHECK(restoreAttempts == 0);
      }
    }

    CHECK(restoreAttempts == 1);
  }

  TEST_CASE("TerminalTitle - sanitizes separator and C1 boundaries while preserving Unicode text",
            "[tui][unit][terminal-title]")
  {
    auto temp = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(temp, std::make_unique<rt::test::QueuedExecutor>());
    auto const track = rt::test::addRuntimeTrack(*runtimePtr, {.title = "A\u2028B\u2029C\u009fD\u00a0E"});
    auto formatter = TerminalTitleFormatter{runtimePtr->library()};
    REQUIRE(formatter.setFormat("$title"));
    CHECK(formatter.format(track) == "A B C D\u00a0E");
    CHECK(formatter.format(track, "\xff") == "A B C D\u00a0E");
    CHECK(formatter.format(track, "\xe2\x80") == "A B C D\u00a0E");

    // Soul text is also an external string at this output boundary.
    CHECK(formatter.format(kInvalidTrackId, "\xff") == "Aobus");
    CHECK(formatter.format(kInvalidTrackId, "\xe2\x80") == "Aobus");
  }

  TEST_CASE("TerminalTitle - bounds wide titles without splitting Unicode or escape delimiters",
            "[tui][unit][terminal-title]")
  {
    auto temp = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(temp, std::make_unique<rt::test::QueuedExecutor>());
    auto longTitle = std::string{};

    for (std::int32_t index = 0; index < 300; ++index)
    {
      longTitle.append("曲");
    }

    auto const track = rt::test::addRuntimeTrack(*runtimePtr, {.title = longTitle});
    auto writes = std::vector<std::string>{};
    auto title = TerminalTitle{runtimePtr->library(),
                               [&](std::string_view value)
                               {
                                 writes.emplace_back(value);
                                 return true;
                               }};
    REQUIRE(title.setFormat("$title"));
    title.update(track);
    REQUIRE(writes.size() == 1);
    auto const prefix = std::string_view{"\033[22;2t\033]2;"};
    auto const suffix = std::string_view{"\033\\"};
    REQUIRE(writes.front().starts_with(prefix));
    REQUIRE(writes.front().ends_with(suffix));
    auto const payload =
      std::string_view{writes.front()}.substr(prefix.size(), writes.front().size() - prefix.size() - suffix.size());
    CHECK(utility::validateUtf8(payload));
    CHECK(cellWidth(payload) == 511);
    CHECK(payload.ends_with("曲…"));
    title.update(track, "⠀⠀⡷");
    REQUIRE(writes.size() == 2);
    auto const animatedPayload = std::string_view{writes.back()}.substr(4, writes.back().size() - 6);
    CHECK(animatedPayload.starts_with("⠀⠀⡷ · 曲"));
    CHECK(animatedPayload.ends_with("曲…"));
    CHECK(utility::validateUtf8(animatedPayload));
    CHECK(cellWidth(animatedPayload) == 511);
    title.update(track, "⠀⠀⡷");
    CHECK(writes.size() == 2);
  }

  TEST_CASE("TerminalTitleFormatter - releases library observation before later publications",
            "[tui][regression][terminal-title]")
  {
    auto temp = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(temp, std::make_unique<rt::test::QueuedExecutor>());
    auto const track = rt::test::addRuntimeTrack(*runtimePtr, {.title = "Song"});
    auto survivingPreview = TerminalTitleFormatter{runtimePtr->library()};
    REQUIRE(survivingPreview.setFormat("$title"));
    CHECK(survivingPreview.format(track) == "Song");

    {
      auto title = TerminalTitleFormatter{runtimePtr->library()};
      REQUIRE(title.setFormat("$title"));
      CHECK(title.format(track) == "Song");
      CHECK(title.format(track, "⠀⠀⡷") == "⠀⠀⡷ · Song");
    }

    rt::test::updateRuntimeTrack(*runtimePtr, track, [](library::test::TrackSpec& spec) { spec.title = "Edited"; });
    CHECK(survivingPreview.format(track, "⠞⠀⠀") == "⠞⠀⠀ · Edited");
  }

  TEST_CASE("TerminalTitleFormatter - draft changes remain independent of the published title",
            "[tui][regression][terminal-title]")
  {
    auto temp = ao::test::TempDir{};
    auto runtimePtr = rt::test::makeRuntime(temp, std::make_unique<rt::test::QueuedExecutor>());
    auto const track = rt::test::addRuntimeTrack(*runtimePtr, {.title = "Song", .artist = "Artist"});
    auto writes = std::vector<std::string>{};
    auto title = TerminalTitle{runtimePtr->library(),
                               [&](std::string_view value)
                               {
                                 writes.emplace_back(value);
                                 return true;
                               }};
    auto preview = TerminalTitleFormatter{runtimePtr->library()};
    REQUIRE(title.setFormat("$title"));
    title.update(track);
    auto const published = writes;
    REQUIRE(preview.setFormat("$artist"));
    CHECK(preview.format(track) == "Artist");
    CHECK(preview.format(track, "⠀⠂⠀") == "⠀⠂⠀ · Artist");
    CHECK(writes == published);
    REQUIRE(preview.setFormat(R"("Draft: " $title)"));
    CHECK(preview.format(track) == "Draft: Song");
    rt::test::updateRuntimeTrack(*runtimePtr, track, [](library::test::TrackSpec& spec) { spec.title = "Edited"; });
    CHECK(preview.format(track) == "Draft: Edited");
    CHECK(writes == published);
    CHECK_FALSE(preview.setFormat("$unknown"));
    CHECK(preview.format(track) == "Draft: Edited");
    REQUIRE(preview.setFormat(""));
    CHECK_FALSE(preview.format(track));
    CHECK(writes == published);
  }
} // namespace ao::tui::test

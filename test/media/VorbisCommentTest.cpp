// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/media/file/detail/VorbisComment.h"

#include <ao/media/VorbisComment.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::media::test
{
  namespace
  {
    void appendU32(std::vector<std::byte>& payload, std::uint32_t value)
    {
      for (std::uint32_t shift = 0; shift < 32; shift += 8)
      {
        payload.push_back(static_cast<std::byte>((value >> shift) & 0xff));
      }
    }

    void appendSized(std::vector<std::byte>& payload, std::string_view value)
    {
      appendU32(payload, static_cast<std::uint32_t>(value.size()));
      auto const bytes = std::as_bytes(std::span<char const>{value.data(), value.size()});
      payload.insert(payload.end(), bytes.begin(), bytes.end());
    }

    std::vector<std::byte> makePayload(std::initializer_list<std::string_view> comments)
    {
      auto payload = std::vector<std::byte>{};
      appendSized(payload, "vendor");
      appendU32(payload, static_cast<std::uint32_t>(comments.size()));

      for (auto const comment : comments)
      {
        appendSized(payload, comment);
      }

      return payload;
    }
  } // namespace

  static_assert(noexcept(
    tryVisitVorbisComments({}, VorbisCommentTrailing::Rejected, [](std::string_view) noexcept { return true; })));
  static_assert(
    !noexcept(tryVisitVorbisComments({}, VorbisCommentTrailing::Rejected, [](std::string_view) { return true; })));

  TEST_CASE("VorbisComment - raw traversal preserves every entry and its order")
  {
    auto const payload = makePayload(
      {"TITLE=First", std::string_view{""}, "separatorless", std::string_view{"KEY=a\0b", 7}, "title=Last"});
    auto visited = std::vector<std::string_view>{};

    bool const complete = tryVisitVorbisComments(payload,
                                                 VorbisCommentTrailing::Rejected,
                                                 [&](std::string_view comment)
                                                 {
                                                   visited.push_back(comment);
                                                   return true;
                                                 });

    CHECK(complete);
    REQUIRE(visited.size() == 5);
    CHECK(visited[0] == std::string_view{"TITLE=First"});
    CHECK(visited[1].empty());
    CHECK(visited[2] == std::string_view{"separatorless"});
    REQUIRE(visited[3].size() == 7);
    CHECK(visited[3] == std::string_view{"KEY=a\0b", 7});
    CHECK(visited[3][5] == '\0');
    CHECK(visited[3][6] == 'b');
    CHECK(visited[4] == std::string_view{"title=Last"});
    // Vendor length, six vendor bytes, count, and first entry length precede the borrowed entry.
    CHECK(visited[0].data() == reinterpret_cast<char const*>(payload.data() + 18));

    auto const optImported = file::detail::parseVorbisComments(payload, VorbisCommentTrailing::Rejected);
    REQUIRE(optImported);
    CHECK(*optImported == visited);
  }

  TEST_CASE("VorbisComment - zero vendor length and zero entries form a complete list")
  {
    auto payload = std::vector<std::byte>{};
    appendU32(payload, 0);
    appendU32(payload, 0);
    auto visited = std::vector<std::string_view>{};

    CHECK(tryVisitVorbisComments(payload,
                                 VorbisCommentTrailing::Rejected,
                                 [&](std::string_view comment)
                                 {
                                   visited.push_back(comment);
                                   return true;
                                 }));
    CHECK(visited.empty());

    auto const optImported = file::detail::parseVorbisComments(payload, VorbisCommentTrailing::Rejected);
    REQUIRE(optImported);
    CHECK(optImported->empty());
  }

  TEST_CASE("VorbisComment - every truncated vendor count or entry field is rejected")
  {
    auto const payload = makePayload({"TITLE=First"});

    for (std::size_t size = 0; size < payload.size(); ++size)
    {
      INFO("Truncated payload size: " << size);
      auto const prefix = std::span<std::byte const>{payload}.first(size);
      auto visited = std::vector<std::string_view>{};

      CHECK_FALSE(tryVisitVorbisComments(prefix,
                                         VorbisCommentTrailing::Rejected,
                                         [&](std::string_view comment)
                                         {
                                           visited.push_back(comment);
                                           return true;
                                         }));
      CHECK(visited.empty());
      CHECK_FALSE(file::detail::parseVorbisComments(prefix, VorbisCommentTrailing::Rejected));
    }
  }

  TEST_CASE("VorbisComment - oversized lengths and counts cannot escape the payload")
  {
    auto payload = std::vector<std::byte>{};

    SECTION("Vendor length exceeds the remaining payload")
    {
      appendU32(payload, std::numeric_limits<std::uint32_t>::max());
      appendU32(payload, 0);
    }

    SECTION("Comment count exceeds the available entry lengths")
    {
      appendU32(payload, 0);
      appendU32(payload, std::numeric_limits<std::uint32_t>::max());
    }

    SECTION("Entry length exceeds the remaining payload")
    {
      appendU32(payload, 0);
      appendU32(payload, 1);
      appendU32(payload, std::numeric_limits<std::uint32_t>::max());
      appendU32(payload, 0);
    }

    auto visited = std::vector<std::string_view>{};
    CHECK_FALSE(tryVisitVorbisComments(payload,
                                       VorbisCommentTrailing::Allowed,
                                       [&](std::string_view comment)
                                       {
                                         visited.push_back(comment);
                                         return true;
                                       }));
    CHECK(visited.empty());
    CHECK_FALSE(file::detail::parseVorbisComments(payload, VorbisCommentTrailing::Allowed));
  }

  TEST_CASE("VorbisComment - malformed suffix exposes a raw prefix but no import result")
  {
    auto payload = makePayload({"TITLE=Visible", "ARTIST=Truncated"});
    payload.pop_back();
    auto visited = std::vector<std::string_view>{};

    CHECK_FALSE(tryVisitVorbisComments(payload,
                                       VorbisCommentTrailing::Rejected,
                                       [&](std::string_view comment)
                                       {
                                         visited.push_back(comment);
                                         return true;
                                       }));
    REQUIRE(visited.size() == 1);
    CHECK(visited[0] == std::string_view{"TITLE=Visible"});
    CHECK_FALSE(file::detail::parseVorbisComments(payload, VorbisCommentTrailing::Rejected));
  }

  TEST_CASE("VorbisComment - trailing bytes follow the explicit FLAC or Opus policy")
  {
    auto payload = makePayload({"TITLE=Visible"});
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0xff});
    auto visited = std::vector<std::string_view>{};
    auto trailing = VorbisCommentTrailing::Rejected;
    bool expectedComplete = false;

    SECTION("FLAC rejects the tail and publishes no import result")
    {
      trailing = VorbisCommentTrailing::Rejected;
    }

    SECTION("Opus allows the tail and publishes the complete list")
    {
      trailing = VorbisCommentTrailing::Allowed;
      expectedComplete = true;
    }

    CHECK(tryVisitVorbisComments(payload,
                                 trailing,
                                 [&](std::string_view comment)
                                 {
                                   visited.push_back(comment);
                                   return true;
                                 }) == expectedComplete);
    REQUIRE(visited.size() == 1);
    CHECK(visited[0] == std::string_view{"TITLE=Visible"});

    auto const optImported = file::detail::parseVorbisComments(payload, trailing);
    REQUIRE(optImported.has_value() == expectedComplete);

    if (optImported)
    {
      CHECK(*optImported == visited);
    }
  }

  TEST_CASE("VorbisComment - a mutable visitor copy is invoked as an lvalue for every entry")
  {
    struct Visitor final
    {
      std::vector<std::pair<std::uint32_t, std::string_view>>& entries;
      std::uint32_t calls = 0;

      bool operator()(std::string_view entry) &
      {
        entries.emplace_back(++calls, entry);
        return true;
      }
    };

    auto const payload = makePayload({"TITLE=First", "TITLE=Second"});
    auto entries = std::vector<std::pair<std::uint32_t, std::string_view>>{};
    auto visitor = Visitor{.entries = entries};
    CHECK(tryVisitVorbisComments(payload, VorbisCommentTrailing::Rejected, visitor));
    CHECK(visitor.calls == 0);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0] == std::pair<std::uint32_t, std::string_view>{1, "TITLE=First"});
    CHECK(entries[1] == std::pair<std::uint32_t, std::string_view>{2, "TITLE=Second"});
  }

  TEST_CASE("VorbisComment - visitor abort returns false without visiting later entries")
  {
    auto const payload = makePayload({"TITLE=First", "TITLE=Second"});
    auto visited = std::vector<std::string_view>{};

    CHECK_FALSE(tryVisitVorbisComments(payload,
                                       VorbisCommentTrailing::Rejected,
                                       [&](std::string_view comment)
                                       {
                                         visited.push_back(comment);
                                         return false;
                                       }));
    REQUIRE(visited.size() == 1);
    CHECK(visited[0] == std::string_view{"TITLE=First"});
  }

  TEST_CASE("VorbisComment - visitor exceptions propagate without visiting later entries")
  {
    auto const payload = makePayload({"TITLE=First", "TITLE=Second"});
    auto visited = std::vector<std::string_view>{};

    REQUIRE_THROWS_AS(tryVisitVorbisComments(payload,
                                             VorbisCommentTrailing::Rejected,
                                             [&](std::string_view comment) -> bool
                                             {
                                               visited.push_back(comment);
                                               throw std::runtime_error{"visitor failure"};
                                             }),
                      std::runtime_error);
    REQUIRE(visited.size() == 1);
    CHECK(visited[0] == std::string_view{"TITLE=First"});
  }

  TEST_CASE("VorbisComment - complete lists above one hundred thousand entries have no artificial cap")
  {
    constexpr std::uint32_t kCount = 100001;
    auto payload = std::vector<std::byte>{};
    appendU32(payload, 0);
    appendU32(payload, kCount);

    for (std::uint32_t index = 0; index < kCount - 1; ++index)
    {
      appendU32(payload, 0);
    }

    appendSized(payload, "LAST=kept");
    std::uint32_t emptyCount = 0;
    auto lastEntry = std::string_view{};

    CHECK(tryVisitVorbisComments(payload,
                                 VorbisCommentTrailing::Rejected,
                                 [&](std::string_view comment) noexcept
                                 {
                                   if (comment.empty())
                                   {
                                     ++emptyCount;
                                   }
                                   else
                                   {
                                     lastEntry = comment;
                                   }

                                   return true;
                                 }));
    CHECK(emptyCount == 100000);
    CHECK(lastEntry == std::string_view{"LAST=kept"});

    auto const optImported = file::detail::parseVorbisComments(payload, VorbisCommentTrailing::Rejected);
    REQUIRE(optImported);
    REQUIRE(optImported->size() == kCount);
    CHECK(optImported->front().empty());
    CHECK(optImported->back() == std::string_view{"LAST=kept"});
  }
} // namespace ao::media::test

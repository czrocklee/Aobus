// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/media/file/detail/Content.h"
#include "lib/media/file/wav/File.h"
#include "test/unit/media/file/TestFile.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/media/file/Visitor.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::media::file::wav::test
{
  namespace
  {
    using Credit = ao::media::file::test::RecordedContent::Credit;

    void appendSyncSafeSize(std::vector<std::uint8_t>& bytes, std::size_t size)
    {
      for (auto const shift : {21U, 14U, 7U, 0U})
      {
        bytes.push_back(static_cast<std::uint8_t>((size >> shift) & 0x7FU));
      }
    }

    std::vector<std::uint8_t> makeTextTag(std::initializer_list<std::pair<std::string_view, std::string_view>> frames,
                                          bool frameUnsynchronised = false)
    {
      auto body = std::vector<std::uint8_t>{};

      for (auto const& [id, text] : frames)
      {
        body.insert(body.end(), id.begin(), id.end());
        appendSyncSafeSize(body, text.size() + 1U);
        body.push_back(0);
        body.push_back(frameUnsynchronised ? 0x02U : 0U);
        body.push_back(0); // Latin-1.
        body.insert(body.end(), text.begin(), text.end());
      }

      auto tag = std::vector<std::uint8_t>{'I', 'D', '3', 4, 0, 0};
      appendSyncSafeSize(tag, body.size());
      tag.insert(tag.end(), body.begin(), body.end());
      return tag;
    }

    bool hasOwnedText(detail::Content const& content, std::string_view text)
    {
      return std::ranges::any_of(content.ownedStrings,
                                 [text](std::string const& owned)
                                 { return owned.data() == text.data() && owned.size() == text.size(); });
    }

    detail::Content contentAfterChunks(std::vector<ao::test::wav::Chunk> chunks)
    {
      auto const bytes = ao::test::wav::makeWav({.extraChunks = std::move(chunks)});
      auto const reader = File{utility::bytes::view(std::span<std::uint8_t const>{bytes})};
      auto contentRes = reader.readContent();
      REQUIRE(contentRes);
      return std::move(*contentRes);
    }

    std::vector<Credit> copiedCredits(detail::Content const& content)
    {
      auto recorded = ao::media::file::test::RecordedContent{};
      auto visitor = ao::media::file::test::VisitorSpy{recorded};
      content.visit(visitor);
      REQUIRE_FALSE(recorded.events().empty());
      CHECK(recorded.events().back().kind == ao::media::file::test::RecordedContent::CallbackKind::Credits);
      return recorded.credits();
    }
  } // namespace

  TEST_CASE("WAV credit content - owns decoded embedded credits after the chunk builder and source bytes die",
            "[media][unit][wav]")
  {
    constexpr auto kCreditText = std::to_array("piano\0Andr\xE9, Ada\0violin\0Bob");
    auto const content = contentAfterChunks(
      {{.id = {'i', 'd', '3', ' '},
        .payload = makeTextTag({{"TMCL", std::string_view{kCreditText.data(), kCreditText.size() - 1}}})}});

    CHECK(copiedCredits(content) == std::vector<Credit>{
                                      {.name = "Andr\xC3\xA9", .kind = CreditKind::Performer, .role = "piano"},
                                      {.name = "Ada", .kind = CreditKind::Performer, .role = "piano"},
                                      {.name = "Bob", .kind = CreditKind::Performer, .role = "violin"},
                                    });

    for (auto const& credit : content.credits)
    {
      CHECK(hasOwnedText(content, credit.name));
      CHECK(hasOwnedText(content, credit.role));
    }
  }

  TEST_CASE("WAV credit content - retains repeats from successive ID3 chunks in order", "[media][unit][wav]")
  {
    constexpr auto kFirstCreditText = std::to_array("piano\0Ada\0piano\0Ada");
    constexpr auto kSecondCreditText = std::to_array("\0Bob");
    auto const content = contentAfterChunks({
      {.id = {'i', 'd', '3', ' '},
       .payload = makeTextTag({{"TMCL", {kFirstCreditText.data(), kFirstCreditText.size() - 1}}})},
      {.id = {'I', 'D', '3', ' '},
       .payload = makeTextTag({{"TMCL", {kSecondCreditText.data(), kSecondCreditText.size() - 1}}})},
    });
    CHECK(copiedCredits(content) == std::vector<Credit>{
                                      {.name = "Ada", .kind = CreditKind::Performer, .role = "piano"},
                                      {.name = "Ada", .kind = CreditKind::Performer, .role = "piano"},
                                      {.name = "Bob", .kind = CreditKind::Performer},
                                    });
  }

  TEST_CASE("WAV credit content - owns text decoded from a frame-unsynchronised TMCL", "[media][unit][wav]")
  {
    constexpr auto kCreditText = std::to_array("piano\0A\xFF\0\xE0");
    auto const content =
      contentAfterChunks({{.id = {'i', 'd', '3', ' '},
                           .payload = makeTextTag({{"TMCL", {kCreditText.data(), kCreditText.size() - 1}}}, true)}});
    CHECK(copiedCredits(content) ==
          std::vector<Credit>{{.name = "A\xC3\xBF\xC3\xA0", .kind = CreditKind::Performer, .role = "piano"}});
    REQUIRE(content.credits.size() == 1);
    CHECK(hasOwnedText(content, content.credits[0].name));
    CHECK(hasOwnedText(content, content.credits[0].role));
  }

  TEST_CASE("WAV credit content - resolves nonblank Ensemble over all Orchestra chunks in both orders",
            "[media][unit][wav]")
  {
    auto const fallback = makeTextTag({{"TXXX", std::string_view{"orchestra\0Fallback A", 20}},
                                       {"TXXX", std::string_view{"orchestra\0Fallback B", 20}}});
    auto const explicitTag = makeTextTag({{"TXXX", std::string_view{"ensemble\0Group A", 16}},
                                          {"TXXX", std::string_view{"ensemble\0 \t\n\r\f\v", 15}},
                                          {"TXXX", std::string_view{"ensemble\0Group B", 16}}});

    for (bool const explicitFirst : {false, true})
    {
      CAPTURE(explicitFirst);
      auto const owned = [&]
      {
        auto const content = contentAfterChunks({
          {.id = {'i', 'd', '3', ' '}, .payload = explicitFirst ? explicitTag : fallback},
          {.id = {'I', 'D', '3', ' '}, .payload = explicitFirst ? fallback : explicitTag},
        });
        return copiedCredits(content);
      }();
      CHECK(owned == std::vector<Credit>{{.name = "Group A", .kind = CreditKind::Ensemble},
                                         {.name = "Group B", .kind = CreditKind::Ensemble}});
    }
  }

  TEST_CASE("WAV credit content - blank explicit Ensemble retains copied fallbacks in cross-chunk traversal order",
            "[media][unit][wav]")
  {
    auto const blank = makeTextTag({{"TXXX", std::string_view{"ensemble\0 \t\n\r\f\v", 15}}});
    auto const first = makeTextTag({{"TXXX", std::string_view{"orchestra\0Andr\xE9", 15}}, {"TPE3", "Conductor"}});
    auto const second = makeTextTag({{"TXXX", std::string_view{"soloist\0Soloist", 15}},
                                     {"TXXX", std::string_view{"orchestra\0Fallback B", 20}},
                                     {"TMCL", std::string_view{"Piano\0Ada", 9}}});

    for (bool const blankFirst : {false, true})
    {
      CAPTURE(blankFirst);
      auto const owned = [&]
      {
        auto chunks = std::vector<ao::test::wav::Chunk>{
          {.id = {'i', 'd', '3', ' '}, .payload = first}, {.id = {'I', 'D', '3', ' '}, .payload = second}};
        chunks.insert(blankFirst ? chunks.begin() : chunks.end(), {.id = {'i', 'd', '3', ' '}, .payload = blank});
        auto const content = contentAfterChunks(std::move(chunks));

        for (auto const& credit : content.credits)
        {
          CHECK(hasOwnedText(content, credit.name));

          if (!credit.role.empty())
          {
            CHECK(hasOwnedText(content, credit.role));
          }
        }

        return copiedCredits(content);
      }();
      CHECK(owned == std::vector<Credit>{
                       {.name = "Andr\xC3\xA9", .kind = CreditKind::Ensemble},
                       {.name = "Conductor", .kind = CreditKind::Conductor},
                       {.name = "Soloist", .kind = CreditKind::Soloist},
                       {.name = "Fallback B", .kind = CreditKind::Ensemble},
                       {.name = "Ada", .kind = CreditKind::Performer, .role = "Piano"},
                     });
    }
  }
} // namespace ao::media::file::wav::test

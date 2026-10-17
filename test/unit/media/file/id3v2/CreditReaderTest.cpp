// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/media/file/detail/Content.h"
#include "lib/media/file/mpeg/id3v2/Layout.h"
#include "lib/media/file/mpeg/id3v2/Reader.h"
#include "test/unit/media/file/TestFile.h"
#include "test/unit/media/file/id3v2/TestId3v2.h"
#include <ao/media/file/Visitor.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::media::file::mpeg::id3v2::test
{
  namespace
  {
    using ao::test::id3v2::appendFrame;
    using ao::test::id3v2::Bytes;
    using ao::test::id3v2::byteSpan;
    using ao::test::id3v2::makeHeader;
    using ao::test::id3v2::textContent;
    using Credit = ao::media::file::test::RecordedContent::Credit;

    std::string nulJoined(std::initializer_list<std::string_view> parts)
    {
      auto text = std::string{};
      bool first = true;

      for (auto const part : parts)
      {
        if (!first)
        {
          text.push_back('\0');
        }

        text.append(part);
        first = false;
      }

      return text;
    }

    Bytes utf16Content(Encoding encoding,
                       bool littleEndian,
                       std::initializer_list<std::string_view> values,
                       bool perValueBom = false)
    {
      auto content = Bytes{std::byte{static_cast<std::uint8_t>(encoding)}};
      auto const appendUnit = [&](std::uint8_t high, std::uint8_t low)
      {
        content.push_back(std::byte{littleEndian ? low : high});
        content.push_back(std::byte{littleEndian ? high : low});
      };
      appendUnit(0xFE, 0xFF);
      bool first = true;

      for (auto const value : values)
      {
        if (!first)
        {
          appendUnit(0, 0);

          if (perValueBom)
          {
            appendUnit(0xFE, 0xFF);
          }
        }

        for (auto const character : value)
        {
          appendUnit(0, static_cast<std::uint8_t>(character));
        }

        first = false;
      }

      return content;
    }

    detail::Content readContent(std::uint8_t majorVersion, Bytes const& body)
    {
      auto optFrames = readFrames(makeHeader(majorVersion, 0x00, body.size()), byteSpan(body));
      REQUIRE(optFrames);
      return std::move(*optFrames).finish();
    }

    std::vector<Credit> readCredits(std::uint8_t majorVersion, Bytes const& body)
    {
      auto const content = readContent(majorVersion, body);
      auto recorded = ao::media::file::test::RecordedContent{};
      auto visitor = ao::media::file::test::VisitorSpy{recorded};
      content.visit(visitor);
      return recorded.credits();
    }
  } // namespace

  TEST_CASE("ID3v2 TMCL - retains pair positions and ignores absent names and unpaired instruments",
            "[media][unit][mpeg][id3v2]")
  {
    struct Example final
    {
      std::string text;
      std::vector<Credit> expected;
    };
    auto const examples = std::array{
      Example{nulJoined({"Guitar", "Keith"}), {{.name = "Keith", .kind = CreditKind::Performer, .role = "Guitar"}}},
      Example{nulJoined({"Guitar", "", "Piano", "Keith"}),
              {{.name = "Keith", .kind = CreditKind::Performer, .role = "Piano"}}},
      Example{nulJoined({"Guitar", "Keith", ""}), {{.name = "Keith", .kind = CreditKind::Performer, .role = "Guitar"}}},
      Example{nulJoined({"", "Keith"}), {{.name = "Keith", .kind = CreditKind::Performer}}},
      Example{nulJoined({"Guitar", " \t"}), {}},
      Example{
        nulJoined({"Guitar", "Keith", "Violin"}), {{.name = "Keith", .kind = CreditKind::Performer, .role = "Guitar"}}},
      Example{"", {}},
      Example{std::string(512, '\0'), {}},
      Example{nulJoined({"\xEF\xBB\xBF", "Anne", "Violin", "Bob"}),
              {{.name = "Anne", .kind = CreditKind::Performer},
               {.name = "Bob", .kind = CreditKind::Performer, .role = "Violin"}}},
      Example{nulJoined({"Piano", "\xEF\xBB\xBF", "Violin", "Bob"}),
              {{.name = "Bob", .kind = CreditKind::Performer, .role = "Violin"}}},
      Example{nulJoined({"Piano\xED\xA0\x80", "Anne\xED\xA0\x80"}),
              {{.name = "Anne\xED\xA0\x80", .kind = CreditKind::Performer, .role = "Piano\xED\xA0\x80"}}},
    };

    for (auto const& example : examples)
    {
      CAPTURE(example.text);
      auto body = Bytes{};
      appendFrame(body, "TMCL", true, 0, textContent(Encoding::Utf8, example.text));
      CHECK(readCredits(4, body) == example.expected);
    }

    auto emptyBody = Bytes{};
    appendFrame(emptyBody, "TMCL", true, 0, textContent(Encoding::Utf8, std::string(512, '\0')));
    CHECK(readContent(4, emptyBody).ownedStrings.empty());

    auto body = Bytes{};
    appendFrame(body, "TMCL", true, 0, textContent(Encoding::Utf8, "Guitar"));
    appendFrame(body, "TMCL", true, 0, textContent(Encoding::Utf8, nulJoined({"Piano", "Anne"})));
    CHECK(readCredits(4, body) ==
          std::vector<Credit>{{.name = "Anne", .kind = CreditKind::Performer, .role = "Piano"}});

    auto unsupportedBody = Bytes{};
    appendFrame(
      unsupportedBody, "TMCL", true, 0, textContent(static_cast<Encoding>(4), nulJoined({"Guitar", "Keith"})));
    CHECK(readCredits(4, unsupportedBody).empty());
  }

  TEST_CASE("ID3v2 TMCL - splits only artist commas and keeps the documented surname ambiguity",
            "[media][unit][mpeg][id3v2]")
  {
    struct Example final
    {
      std::string_view instrument;
      std::string_view artists;
      std::vector<Credit> expected;
    };
    auto const examples = std::array{
      Example{"Guitar",
              "Keith, Don",
              {{.name = "Keith", .kind = CreditKind::Performer, .role = "Guitar"},
               {.name = "Don", .kind = CreditKind::Performer, .role = "Guitar"}}},
      Example{"Guitar",
              "A,, B , ,C",
              {{.name = "A", .kind = CreditKind::Performer, .role = "Guitar"},
               {.name = "B", .kind = CreditKind::Performer, .role = "Guitar"},
               {.name = "C", .kind = CreditKind::Performer, .role = "Guitar"}}},
      Example{"Guitar", "Keith,", {{.name = "Keith", .kind = CreditKind::Performer, .role = "Guitar"}}},
      Example{
        "Piano, Organ", "Anne; Bob", {{.name = "Anne; Bob", .kind = CreditKind::Performer, .role = "Piano, Organ"}}},
      Example{"Guitar", ", ,", {}},
      Example{" Guitar \t",
              "  Anne ,  Bob  ",
              {{.name = "Anne", .kind = CreditKind::Performer, .role = "Guitar"},
               {.name = "Bob", .kind = CreditKind::Performer, .role = "Guitar"}}},
      Example{
        " \t\n\r\f\vGuitar \t\n\r\f\v", "Anne", {{.name = "Anne", .kind = CreditKind::Performer, .role = "Guitar"}}},
      // The source syntax cannot distinguish a surname comma from an artist separator.
      Example{"Violin",
              "Smith, John",
              {{.name = "Smith", .kind = CreditKind::Performer, .role = "Violin"},
               {.name = "John", .kind = CreditKind::Performer, .role = "Violin"}}},
    };

    for (auto const& example : examples)
    {
      CAPTURE(example.instrument, example.artists);
      auto body = Bytes{};
      appendFrame(body, "TMCL", true, 0, textContent(Encoding::Utf8, nulJoined({example.instrument, example.artists})));
      CHECK(readCredits(4, body) == example.expected);
    }
  }

  TEST_CASE("ID3v2 TMCL - decodes supported encodings and per-value BOMs without shifting empty positions",
            "[media][unit][mpeg][id3v2]")
  {
    auto const inputs = std::array{
      textContent(Encoding::Latin1,
                  nulJoined({"Guitar",
                             std::string_view{"Ren\xE9"
                                              "e",
                                              5}})),
      utf16Content(Encoding::Utf16Be, false, {"Piano", "Anne"}),
      utf16Content(Encoding::Ucs2, true, {"Piano", "Anne"}),
      utf16Content(Encoding::Ucs2, true, {"Piano", "", "Violin", "Bob"}),
      textContent(Encoding::Utf8,
                  nulJoined({"Guitar",
                             std::string_view{"\xEF\xBB\xBF"
                                              "Anne",
                                              7}})),
    };
    auto const expected = std::array{
      Credit{.name = "Ren\xC3\xA9"
                     "e",
             .kind = CreditKind::Performer,
             .role = "Guitar"},
      Credit{.name = "Anne", .kind = CreditKind::Performer, .role = "Piano"},
      Credit{.name = "Anne", .kind = CreditKind::Performer, .role = "Piano"},
      Credit{.name = "Bob", .kind = CreditKind::Performer, .role = "Violin"},
      Credit{.name = "Anne", .kind = CreditKind::Performer, .role = "Guitar"},
    };

    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
      CAPTURE(index);
      auto body = Bytes{};
      appendFrame(body, "TMCL", true, 0, inputs[index]);
      CHECK(readCredits(4, body) == std::vector<Credit>{expected[index]});
    }
  }

  TEST_CASE(
    "ID3v2 TMCL - shared decoding preserves empty positions comma pairing and trailing values in every encoding",
    "[media][unit][mpeg][id3v2]")
  {
    auto const values = std::initializer_list<std::string_view>{
      "", "Anne", "Piano", "", "Guitar, Bass", "Bob, Cara", "Violin", "Dan", "Odd role", "", ""};
    auto const text = nulJoined(values);
    auto const inputs = std::array{
      textContent(Encoding::Latin1, text),
      textContent(Encoding::Utf8, text),
      utf16Content(Encoding::Ucs2, true, values),
      utf16Content(Encoding::Ucs2, false, values),
      utf16Content(Encoding::Utf16Be, false, values),
      utf16Content(Encoding::Ucs2, true, values, true),
      utf16Content(Encoding::Utf16Be, false, values, true),
    };

    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
      CAPTURE(index);
      auto body = Bytes{};
      appendFrame(body, "TMCL", true, 0, inputs[index]);
      CHECK(readCredits(4, body) == std::vector<Credit>{
                                      {.name = "Anne", .kind = CreditKind::Performer},
                                      {.name = "Bob", .kind = CreditKind::Performer, .role = "Guitar, Bass"},
                                      {.name = "Cara", .kind = CreditKind::Performer, .role = "Guitar, Bass"},
                                      {.name = "Dan", .kind = CreditKind::Performer, .role = "Violin"},
                                    });
    }
  }

  TEST_CASE("ID3v2 TMCL - UTF-16 odd byte tails do not create a name or displace the next frame",
            "[media][unit][mpeg][id3v2]")
  {
    for (bool const littleEndian : {false, true})
    {
      CAPTURE(littleEndian);
      auto content = utf16Content(Encoding::Ucs2, littleEndian, {"", "Anne", "Unpaired"}, true);
      content.push_back(std::byte{0x41});
      auto body = Bytes{};
      appendFrame(body, "TMCL", true, 0, content);
      appendFrame(body, "TMCL", true, 0, utf16Content(Encoding::Ucs2, littleEndian, {"Violin", "Bob"}, true));
      CHECK(readCredits(4, body) == std::vector<Credit>{
                                      {.name = "Anne", .kind = CreditKind::Performer},
                                      {.name = "Bob", .kind = CreditKind::Performer, .role = "Violin"},
                                    });
    }
  }

  TEST_CASE("ID3v2 TMCL - is v2.4-only", "[media][unit][mpeg][id3v2]")
  {
    auto oldBody = Bytes{};
    appendFrame(oldBody, "TMCL", false, 0, textContent(Encoding::Latin1, nulJoined({"Guitar", "Keith"})));
    CHECK(readCredits(3, oldBody).empty());
  }

  TEST_CASE("ID3v2 TIPL and IPLS - map Picard involvements to credit kinds", "[media][unit][mpeg][id3v2]")
  {
    // Matching ignores ASCII case and surrounding whitespace. Other involvements,
    // empty names, and an odd final involvement contribute nothing.
    auto const text = nulJoined({"arranger",
                                 "Ann",
                                 " Producer ",
                                 "Smith, John",
                                 "composer",
                                 "Carl",
                                 "engineer",
                                 "",
                                 "ENGINEER",
                                 "Eve",
                                 "mix",
                                 "Max",
                                 "DJ-mix",
                                 "Dee",
                                 "producer"});
    auto const expected = std::vector<Credit>{
      {.name = "Ann", .kind = CreditKind::Arranger},
      {.name = "Smith, John", .kind = CreditKind::Producer},
      {.name = "Eve", .kind = CreditKind::Engineer},
      {.name = "Max", .kind = CreditKind::Mixer},
      {.name = "Dee", .kind = CreditKind::DjMixer},
    };

    SECTION("TIPL in a v2.4 tag")
    {
      auto body = Bytes{};
      appendFrame(body, "TIPL", true, 0, textContent(Encoding::Utf8, text));
      CHECK(readCredits(4, body) == expected);
    }

    SECTION("IPLS in a v2.3 tag splits every NUL even though v2.3 text frames hold one value")
    {
      auto body = Bytes{};
      appendFrame(body, "IPLS", false, 0, textContent(Encoding::Latin1, text));
      CHECK(readCredits(3, body) == expected);
    }

    SECTION("IPLS in UCS-2 with a BOM on every string")
    {
      auto body = Bytes{};
      appendFrame(body, "IPLS", false, 0, utf16Content(Encoding::Ucs2, true, {"producer", "Ann", "mix", "Max"}, true));
      CHECK(readCredits(3, body) == std::vector<Credit>{{.name = "Ann", .kind = CreditKind::Producer},
                                                        {.name = "Max", .kind = CreditKind::Mixer}});
    }

    SECTION("each frame is read only in its own version")
    {
      auto oldBody = Bytes{};
      appendFrame(oldBody, "TIPL", false, 0, textContent(Encoding::Latin1, text));
      CHECK(readCredits(3, oldBody).empty());

      auto newBody = Bytes{};
      appendFrame(newBody, "IPLS", true, 0, textContent(Encoding::Utf8, text));
      CHECK(readCredits(4, newBody).empty());
    }
  }

  TEST_CASE("ID3v2 credits - TEXT TPE4 and TXXX Writer name lyricists remixers and writers",
            "[media][unit][mpeg][id3v2]")
  {
    SECTION("v2.4 frames apply each NUL-separated value")
    {
      auto body = Bytes{};
      appendFrame(body, "TEXT", true, 0, textContent(Encoding::Utf8, nulJoined({"Lee", " ", "Lou"})));
      appendFrame(body, "TPE4", true, 0, textContent(Encoding::Utf8, "Rex"));
      appendFrame(body, "TXXX", true, 0, textContent(Encoding::Utf8, nulJoined({"WRITER", "Wes", "Wyn"})));
      CHECK(readCredits(4, body) == std::vector<Credit>{
                                      {.name = "Lee", .kind = CreditKind::Lyricist},
                                      {.name = "Lou", .kind = CreditKind::Lyricist},
                                      {.name = "Rex", .kind = CreditKind::Remixer},
                                      {.name = "Wes", .kind = CreditKind::Writer},
                                      {.name = "Wyn", .kind = CreditKind::Writer},
                                    });
    }

    SECTION("v2.3 frames stop at the first terminator without splitting slash text")
    {
      auto body = Bytes{};
      appendFrame(body, "TEXT", false, 0, textContent(Encoding::Latin1, std::string_view{"A/B\0C", 5}));
      appendFrame(body, "TXXX", false, 0, textContent(Encoding::Latin1, std::string_view{"Writer\0W/X\0Y", 12}));
      CHECK(readCredits(3, body) == std::vector<Credit>{{.name = "A/B", .kind = CreditKind::Lyricist},
                                                        {.name = "W/X", .kind = CreditKind::Writer}});
    }
  }

  TEST_CASE("ID3v2 credits - v2.3 UTF-16 TXXX values drop the BOM of their own string", "[media][unit][mpeg][id3v2]")
  {
    // Picard and Mutagen encode the description and the value as separate
    // UTF-16 strings, each with a BOM; v2.3 still reads only the first value.
    for (bool const littleEndian : {true, false})
    {
      CAPTURE(littleEndian);
      auto body = Bytes{};
      appendFrame(body, "TXXX", false, 0, utf16Content(Encoding::Ucs2, littleEndian, {"Writer", "Wes", "Wyn"}, true));
      appendFrame(body, "TXXX", false, 0, utf16Content(Encoding::Ucs2, littleEndian, {"CONDUCTOR", "Carl"}, true));
      appendFrame(body, "TXXX", false, 0, utf16Content(Encoding::Ucs2, littleEndian, {"soloist", "Sue"}, true));
      CHECK(readCredits(3, body) == std::vector<Credit>{{.name = "Wes", .kind = CreditKind::Writer},
                                                        {.name = "Carl", .kind = CreditKind::Conductor},
                                                        {.name = "Sue", .kind = CreditKind::Soloist}});
    }
  }

  TEST_CASE("ID3v2 TMCL - preserves duplicates and order within and across frames", "[media][unit][mpeg][id3v2]")
  {
    auto body = Bytes{};
    appendFrame(
      body, "TMCL", true, 0, textContent(Encoding::Utf8, nulJoined({"Guitar", "A", "Guitar", "B", "Violin", "A"})));
    appendFrame(body, "TMCL", true, 0, textContent(Encoding::Utf8, nulJoined({"Guitar", "A"})));
    appendFrame(body, "TMCL", true, 0, textContent(Encoding::Utf8, nulJoined({"Violin", "B"})));
    CHECK(readCredits(4, body) == std::vector<Credit>{
                                    {.name = "A", .kind = CreditKind::Performer, .role = "Guitar"},
                                    {.name = "B", .kind = CreditKind::Performer, .role = "Guitar"},
                                    {.name = "A", .kind = CreditKind::Performer, .role = "Violin"},
                                    {.name = "A", .kind = CreditKind::Performer, .role = "Guitar"},
                                    {.name = "B", .kind = CreditKind::Performer, .role = "Violin"},
                                  });
  }

  TEST_CASE("ID3v2 TMCL - never implies Soloist and explicit soloist remains independent", "[media][unit][mpeg][id3v2]")
  {
    auto body = Bytes{};
    appendFrame(body, "TMCL", true, 0, textContent(Encoding::Utf8, nulJoined({"Guitar", "Keith"})));
    CHECK(readCredits(4, body) ==
          std::vector<Credit>{{.name = "Keith", .kind = CreditKind::Performer, .role = "Guitar"}});
    appendFrame(body, "TXXX", true, 0, textContent(Encoding::Utf8, nulJoined({"soloist", "Anne"})));
    CHECK(readCredits(4, body) == std::vector<Credit>{
                                    {.name = "Keith", .kind = CreditKind::Performer, .role = "Guitar"},
                                    {.name = "Anne", .kind = CreditKind::Soloist},
                                  });
  }

  TEST_CASE("ID3v2 credits - TPE3 and supported TXXX mappings accumulate v2.4 NUL values without slash splitting",
            "[media][unit][mpeg][id3v2]")
  {
    auto body = Bytes{};
    appendFrame(body, "TPE3", true, 0, textContent(Encoding::Utf8, nulJoined({"A/B", "", " A/B ", " \t"})));
    appendFrame(body, "TXXX", true, 0, textContent(Encoding::Utf8, nulJoined({"conductor", "C,D", "C,D"})));
    appendFrame(body, "TXXX", true, 0, textContent(Encoding::Utf8, nulJoined({"orchestra", "Fallback", "Fallback 2"})));
    appendFrame(body, "TXXX", true, 0, textContent(Encoding::Utf8, nulJoined({"soloist", "Solo/A", "Solo B"})));
    appendFrame(body, "TXXX", true, 0, textContent(Encoding::Utf8, nulJoined({"ensemble", "Group A", " ", "Group B"})));
    appendFrame(body, "TPE3", true, 0, utf16Content(Encoding::Ucs2, true, {"D", "E"}));
    CHECK(readCredits(4, body) == std::vector<Credit>{
                                    {.name = "A/B", .kind = CreditKind::Conductor},
                                    {.name = "A/B", .kind = CreditKind::Conductor},
                                    {.name = "C,D", .kind = CreditKind::Conductor},
                                    {.name = "C,D", .kind = CreditKind::Conductor},
                                    {.name = "Solo/A", .kind = CreditKind::Soloist},
                                    {.name = "Solo B", .kind = CreditKind::Soloist},
                                    {.name = "Group A", .kind = CreditKind::Ensemble},
                                    {.name = "Group B", .kind = CreditKind::Ensemble},
                                    {.name = "D", .kind = CreditKind::Conductor},
                                    {.name = "E", .kind = CreditKind::Conductor},
                                  });
  }

  TEST_CASE("ID3v2 credits - TPE3 and TXXX retain version-aware multivalue encoding and per-value BOM decoding",
            "[media][unit][mpeg][id3v2]")
  {
    struct Example final
    {
      std::string_view frame;
      Bytes content;
      std::vector<Credit> expected;
    };
    auto const examples = std::array{
      Example{"TPE3",
              textContent(Encoding::Latin1,
                          nulJoined({std::string_view{"Ren\xE9"
                                                      "e",
                                                      5},
                                     "Bob"})),
              {{.name = "Ren\xC3\xA9"
                        "e",
                .kind = CreditKind::Conductor},
               {.name = "Bob", .kind = CreditKind::Conductor}}},
      Example{"TPE3",
              utf16Content(Encoding::Utf16Be, false, {"A", "B"}),
              {{.name = "A", .kind = CreditKind::Conductor}, {.name = "B", .kind = CreditKind::Conductor}}},
      Example{"TXXX",
              utf16Content(Encoding::Ucs2, true, {"soloist", "Anne", "Bob"}),
              {{.name = "Anne", .kind = CreditKind::Soloist}, {.name = "Bob", .kind = CreditKind::Soloist}}},
      Example{"TXXX",
              textContent(Encoding::Utf8,
                          nulJoined({"conductor",
                                     "A",
                                     "\xEF\xBB\xBF"
                                     "B"})),
              {{.name = "A", .kind = CreditKind::Conductor}, {.name = "B", .kind = CreditKind::Conductor}}},
    };

    for (auto const& example : examples)
    {
      CAPTURE(example.frame);
      auto body = Bytes{};
      appendFrame(body, example.frame, true, 0, example.content);
      CHECK(readCredits(4, body) == example.expected);
    }
  }

  TEST_CASE("ID3v2 credits - Ensemble whole-tag precedence is nonblank and independent of order",
            "[media][unit][mpeg][id3v2]")
  {
    for (bool const explicitFirst : {false, true})
    {
      for (bool const blankExplicit : {false, true})
      {
        CAPTURE(explicitFirst, blankExplicit);
        auto const explicitFrame =
          textContent(Encoding::Utf8, nulJoined({"ensemble", blankExplicit ? " \t\n\r\f\v" : "Group", ""}));
        auto const fallbackFrame =
          textContent(Encoding::Utf8, nulJoined({"orchestra", "Fallback A", "", "Fallback B"}));
        auto body = Bytes{};
        appendFrame(body, "TXXX", true, 0, explicitFirst ? explicitFrame : fallbackFrame);
        appendFrame(body, "TPE3", true, 0, textContent(Encoding::Utf8, "Conductor"));
        appendFrame(body, "TXXX", true, 0, explicitFirst ? fallbackFrame : explicitFrame);

        if (auto const credits = readCredits(4, body); blankExplicit && explicitFirst)
        {
          CHECK(credits == std::vector<Credit>{
                             {.name = "Conductor", .kind = CreditKind::Conductor},
                             {.name = "Fallback A", .kind = CreditKind::Ensemble},
                             {.name = "Fallback B", .kind = CreditKind::Ensemble},
                           });
        }
        else if (blankExplicit)
        {
          CHECK(credits == std::vector<Credit>{
                             {.name = "Fallback A", .kind = CreditKind::Ensemble},
                             {.name = "Fallback B", .kind = CreditKind::Ensemble},
                             {.name = "Conductor", .kind = CreditKind::Conductor},
                           });
        }
        else if (explicitFirst)
        {
          CHECK(credits == std::vector<Credit>{
                             {.name = "Group", .kind = CreditKind::Ensemble},
                             {.name = "Conductor", .kind = CreditKind::Conductor},
                           });
        }
        else
        {
          CHECK(credits == std::vector<Credit>{
                             {.name = "Conductor", .kind = CreditKind::Conductor},
                             {.name = "Group", .kind = CreditKind::Ensemble},
                           });
        }
      }
    }
  }
} // namespace ao::media::file::mpeg::id3v2::test

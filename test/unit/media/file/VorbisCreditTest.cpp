// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/media/file/detail/Content.h"
#include "lib/media/file/detail/VorbisComment.h"
#include "test/unit/media/file/TestFile.h"
#include <ao/media/file/Visitor.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::media::file::vorbis::test
{
  namespace
  {
    using namespace ao::media::file::detail;
    using Credit = ao::media::file::test::RecordedContent::Credit;

    Content contentAfterComments(std::initializer_list<std::pair<std::string_view, std::string_view>> comments)
    {
      auto builder = ContentBuilder::makeEmpty();

      for (auto const& [key, value] : comments)
      {
        CHECK(tryApplyVorbisComment(builder, VorbisCommentField{.key = key, .value = value}));
      }

      return std::move(builder).finish();
    }

    std::vector<Credit> creditsAfterComments(
      std::initializer_list<std::pair<std::string_view, std::string_view>> comments)
    {
      auto const content = contentAfterComments(comments);
      auto recorded = ao::media::file::test::RecordedContent{};
      auto visitor = ao::media::file::test::VisitorSpy{recorded};
      content.visit(visitor);
      return recorded.credits();
    }
  } // namespace

  TEST_CASE("VorbisComment - PERFORMER parses one final isolated role suffix", "[media][unit][vorbis-comment]")
  {
    struct Example final
    {
      std::string_view input;
      std::string_view name;
      std::string_view role;
    };
    auto const examples = std::array{
      Example{"John Smith (Piano)", "John Smith", "Piano"},
      Example{"John Smith \t\r\n\f\v(Piano)", "John Smith", "Piano"},
      Example{"John ( Piano )", "John", "Piano"},
      Example{"  Anne  (Violin)  ", "Anne", "Violin"},
      Example{"John Smith  (Piano)", "John Smith", "Piano"},
      Example{"John (Lead Vocal)", "John", "Lead Vocal"},
      Example{"Smith, John; Ada (Piano)", "Smith, John; Ada", "Piano"},
    };

    for (auto const& example : examples)
    {
      CAPTURE(example.input);
      auto const credits = creditsAfterComments({{"PERFORMER", example.input}});
      REQUIRE(credits.size() == 1);
      CHECK(credits[0].name == example.name);
      CHECK(credits[0].kind == CreditKind::Performer);
      CHECK(credits[0].role == example.role);
    }
  }

  TEST_CASE("VorbisComment - PERFORMER falls back to one whole unstructured name", "[media][unit][vorbis-comment]")
  {
    auto const examples = std::to_array<std::string_view>({
      "John Smith",
      "John ()",
      "John ( \t\r\n\f\v )",
      "(John Smith)",
      "John ((Piano))",
      "John (X) (Piano)",
      "Smith, John",
      "John(Piano)",
      "John (Piano) extra",
      "John\u00A0(Piano)",
      "John （Piano）",
    });

    for (auto const example : examples)
    {
      CAPTURE(example);
      auto const credits = creditsAfterComments({{"PERFORMER", example}});
      REQUIRE(credits.size() == 1);
      CHECK(credits[0].name == example);
      CHECK(credits[0].kind == CreditKind::Performer);
      CHECK(credits[0].role.empty());
    }

    CHECK(creditsAfterComments({{"PERFORMER", "  John Smith  "}}) ==
          std::vector<Credit>{{.name = "John Smith", .kind = CreditKind::Performer}});
  }

  TEST_CASE("VorbisComment - blank PERFORMER contributes no credit", "[media][unit][vorbis-comment]")
  {
    CHECK(creditsAfterComments({{"PERFORMER", ""}}).empty());
    CHECK(creditsAfterComments({{"PERFORMER", " \t\n\r\f\v"}}).empty());
  }

  TEST_CASE("VorbisComment - PERFORMER never implies Soloist", "[media][unit][vorbis-comment]")
  {
    CHECK(creditsAfterComments({{"PERFORMER", "  Anne (Piano)  "}}) ==
          std::vector<Credit>{{.name = "Anne", .kind = CreditKind::Performer, .role = "Piano"}});
    CHECK(creditsAfterComments({{"SOLOIST", "Anne"}, {"PERFORMER", "Bob (Piano)"}}) ==
          std::vector<Credit>{
            {.name = "Anne", .kind = CreditKind::Soloist},
            {.name = "Bob", .kind = CreditKind::Performer, .role = "Piano"},
          });
    CHECK(creditsAfterComments({{"PERFORMER", "Bob"}, {"SOLOIST", "Anne"}}) ==
          std::vector<Credit>{
            {.name = "Bob", .kind = CreditKind::Performer},
            {.name = "Anne", .kind = CreditKind::Soloist},
          });
  }

  TEST_CASE("VorbisComment - repeated category comments accumulate in traversal order", "[media][unit][vorbis-comment]")
  {
    CHECK(creditsAfterComments({{"ARTIST", "Not a credit"},
                                {"PERFORMER", "Bob (Piano)"},
                                {"CONDUCTOR", "Anne"},
                                {"PERFORMER", "Bob"},
                                {"SOLOIST", "Solo"},
                                {"ENSEMBLE", "Group"},
                                {"CONDUCTOR", "Anne"},
                                {"PERFORMER", "Bob (Piano)"},
                                {"PERFORMER", "Anne"},
                                {"SOLOIST", "Solo"},
                                {"ENSEMBLE", "Group"},
                                {"CONDUCTOR", " \t\r\n\f\v"},
                                {"SOLOIST", ""},
                                {"ENSEMBLE", " "}}) ==
          std::vector<Credit>{
            {.name = "Bob", .kind = CreditKind::Performer, .role = "Piano"},
            {.name = "Anne", .kind = CreditKind::Conductor},
            {.name = "Bob", .kind = CreditKind::Performer},
            {.name = "Solo", .kind = CreditKind::Soloist},
            {.name = "Group", .kind = CreditKind::Ensemble},
            {.name = "Anne", .kind = CreditKind::Conductor},
            {.name = "Bob", .kind = CreditKind::Performer, .role = "Piano"},
            {.name = "Anne", .kind = CreditKind::Performer},
            {.name = "Solo", .kind = CreditKind::Soloist},
            {.name = "Group", .kind = CreditKind::Ensemble},
          });
  }

  TEST_CASE("VorbisComment - nonblank Ensemble discards all Orchestra fallbacks across the whole content",
            "[media][unit][vorbis-comment]")
  {
    auto const explicitCredits = std::vector<Credit>{{.name = "Group A", .kind = CreditKind::Ensemble},
                                                     {.name = "Ada", .kind = CreditKind::Performer},
                                                     {.name = "Group B", .kind = CreditKind::Ensemble}};
    CHECK(creditsAfterComments({{"ORCHESTRA", "Fallback A"},
                                {"ENSEMBLE", " Group A "},
                                {"PERFORMER", "Ada"},
                                {"ORCHESTRA", "Fallback B"},
                                {"ENSEMBLE", "Group B"},
                                {"ENSEMBLE", " "}}) == explicitCredits);
    CHECK(creditsAfterComments({{"ENSEMBLE", "Group A"},
                                {"ORCHESTRA", "Fallback A"},
                                {"PERFORMER", "Ada"},
                                {"ENSEMBLE", "Group B"},
                                {"ORCHESTRA", "Fallback B"}}) == explicitCredits);
    auto const fallbackCredits = std::vector<Credit>{{.name = "Fallback A", .kind = CreditKind::Ensemble},
                                                     {.name = "Ada", .kind = CreditKind::Performer},
                                                     {.name = "Fallback B", .kind = CreditKind::Ensemble}};
    CHECK(creditsAfterComments({{"ENSEMBLE", " \t\n\r\f\v"},
                                {"ORCHESTRA", "Fallback A"},
                                {"PERFORMER", "Ada"},
                                {"ORCHESTRA", "Fallback B"},
                                {"ENSEMBLE", ""},
                                {"ORCHESTRA", " "}}) == fallbackCredits);
    CHECK(creditsAfterComments({{"ORCHESTRA", "Fallback A"},
                                {"ENSEMBLE", " "},
                                {"PERFORMER", "Ada"},
                                {"ORCHESTRA", "Fallback B"},
                                {"ENSEMBLE", ""}}) == fallbackCredits);
  }

  TEST_CASE("VorbisComment - preserves malformed UTF-8 for later admission failure", "[media][unit][vorbis-comment]")
  {
    CHECK(creditsAfterComments({{"PERFORMER", "John\xED\xA0\x80 (Piano)"}}) ==
          std::vector<Credit>{{.name = "John\xED\xA0\x80", .kind = CreditKind::Performer, .role = "Piano"}});
  }

  TEST_CASE("VorbisComment - Artist and Composer do not become performance credits", "[media][unit][vorbis-comment]")
  {
    CHECK(creditsAfterComments({{"ARTIST", "John Smith (Piano)"}, {"COMPOSER", "Composer"}}).empty());
  }
} // namespace ao::media::file::vorbis::test

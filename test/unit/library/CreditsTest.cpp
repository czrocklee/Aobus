// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/Credits.h>

#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    Result<std::vector<Credit>> normalizeList(std::initializer_list<CreditView> entries)
    {
      return normalizeCredits(std::span<CreditView const>{entries});
    }
  }

  TEST_CASE("Credits - builder copies descriptors but borrows unadmitted owning text", "[library][unit][credits]")
  {
    auto const malformed = std::string{"\xC0\xAF", 2};
    auto const entries = std::vector<Credit>{{.name = " Cafe\u0301 ", .role = "\t "},
                                             {.name = " ", .role = malformed},
                                             {.name = " Cafe\u0301 ", .role = "\t "}};
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().credits(entries);
    auto const views = builder.metadata().credits();
    REQUIRE(views.size() == 3);

    for (std::size_t index = 0; index < entries.size(); ++index)
    {
      CHECK(views[index].name == entries[index].name);
      CHECK(views[index].role == entries[index].role);
      CHECK(views[index].kind == entries[index].kind);
      CHECK(views[index].name.data() == entries[index].name.data());
      CHECK(views[index].role.data() == entries[index].role.data());
    }

    auto const res = normalizeCredits(views);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(res.error().message.contains("Credit entry 1"));
  }

  TEST_CASE("Credits - normalized owning text outlives input and preserves duplicates", "[library][unit][credits]")
  {
    auto const res = []
    {
      auto const entries = std::vector<Credit>{{.name = " Cafe\u0301 ", .role = " Pianoforte\u0301 "},
                                               {.name = "Other", .role = " \t\r\n\f\v"},
                                               {.name = " Cafe\u0301 ", .role = " Pianoforte\u0301 "}};
      return normalizeCredits(entries);
    }();
    REQUIRE(res);
    CHECK(*res == std::vector<Credit>{
                    {.name = "Café", .role = "Pianoforté"}, {.name = "Other"}, {.name = "Café", .role = "Pianoforté"}});
  }

  TEST_CASE("Credits - trims six ASCII whitespace characters but not NBSP", "[library][unit][credits]")
  {
    auto const res =
      normalizeList({{.name = " \t\n\r\f\v Glenn Gould \v\f\r\n\t ", .role = " \t\n\r\f\v Piano \v\f\r\n\t "},
                     {.name = "\u00A0Gould\u00A0", .role = "\u00A0Piano\u00A0"},
                     {.name = "\u00A0", .role = "\u00A0"}});
    REQUIRE(res);
    CHECK(*res == std::vector<Credit>{{.name = "Glenn Gould", .role = "Piano"},
                                      {.name = "\u00A0Gould\u00A0", .role = "\u00A0Piano\u00A0"},
                                      {.name = "\u00A0", .role = "\u00A0"}});
  }

  TEST_CASE("Credits - NFC equivalent names and roles compare equal", "[library][unit][credits]")
  {
    auto const decomposedRes = normalizeList({{.name = "Cafe\u0301", .role = "Pianoforte\u0301"}});
    auto const composedRes = normalizeList({{.name = "Café", .role = "Pianoforté"}});
    REQUIRE(decomposedRes);
    REQUIRE(composedRes);
    CHECK(*decomposedRes == *composedRes);
    CHECK(*decomposedRes == std::vector<Credit>{{.name = "Café", .role = "Pianoforté"}});
  }

  TEST_CASE("Credits - malformed scalar UTF-8 rejects the whole list", "[library][unit][credits]")
  {
    auto const malformed = std::vector<std::string>{
      "\xC0\xAF", "\xED\xA0\x80", "\xE2\x82", "\x80", "\xF4\x90\x80\x80", "\xF8\x88\x80\x80\x80"};

    for (auto const& text : malformed)
    {
      auto const nameRes = normalizeList({{.name = "Valid"}, {.name = text, .role = "Piano"}});
      REQUIRE_FALSE(nameRes);
      CHECK(nameRes.error().code == Error::Code::InvalidInput);
      CHECK(nameRes.error().message.contains("Credit entry 1 name"));
      auto const roleRes = normalizeList({{.name = "Valid", .role = text}});
      REQUIRE_FALSE(roleRes);
      CHECK(roleRes.error().code == Error::Code::InvalidInput);
      CHECK(roleRes.error().message.contains("Credit entry 0 role"));
    }
  }

  TEST_CASE("Credits - empty lists are valid and authored blank names reject", "[library][unit][credits]")
  {
    auto const emptyRes = normalizeList({});
    REQUIRE(emptyRes);
    CHECK(emptyRes->empty());

    for (auto const* const name : {"", " ", "\t", "\r\n", " \t\n\r\f\v", "\f \v"})
    {
      auto const res = normalizeList({{.name = "Valid"}, {.name = name, .role = "Piano"}});
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(res.error().message.contains("Credit entry 1"));
    }
  }

  TEST_CASE("Credits - blank roles mean absence and delimiters remain literal", "[library][unit][credits]")
  {
    auto const res = normalizeList({{.name = "Glenn Gould", .role = ""},
                                    {.name = "Jane Smith", .role = " "},
                                    {.name = "Doe", .role = " \t\n\r\f\v "},
                                    {.name = "John Smith (Piano)", .role = "Piano; Violin"},
                                    {.name = "Duo A, Duo B", .role = "(Ensemble)"}});
    REQUIRE(res);
    CHECK(*res == std::vector<Credit>{{.name = "Glenn Gould"},
                                      {.name = "Jane Smith"},
                                      {.name = "Doe"},
                                      {.name = "John Smith (Piano)", .role = "Piano; Violin"},
                                      {.name = "Duo A, Duo B", .role = "(Ensemble)"}});
  }

  TEST_CASE("Credits - stable fixed-kind grouping preserves multiplicity and within-kind order",
            "[library][unit][credits]")
  {
    auto const res = normalizeList({{.name = "P", .role = "soloist"},
                                    {.name = "S", .kind = CreditKind::Soloist},
                                    {.name = "C2", .kind = CreditKind::Conductor},
                                    {.name = "E", .kind = CreditKind::Ensemble},
                                    {.name = "C1", .kind = CreditKind::Conductor},
                                    {.name = "C2", .kind = CreditKind::Conductor},
                                    {.name = "P", .role = "soloist"}});
    REQUIRE(res);
    CHECK(*res == std::vector<Credit>{{.name = "C2", .kind = CreditKind::Conductor},
                                      {.name = "C1", .kind = CreditKind::Conductor},
                                      {.name = "C2", .kind = CreditKind::Conductor},
                                      {.name = "E", .kind = CreditKind::Ensemble},
                                      {.name = "S", .kind = CreditKind::Soloist},
                                      {.name = "P", .role = "soloist"},
                                      {.name = "P", .role = "soloist"}});
    auto changed = *res;
    std::swap(changed[0], changed[1]);
    CHECK(changed != *res);
    changed = *res;
    changed.back().role.clear();
    CHECK(changed != *res);
    changed = *res;
    changed.back().kind = CreditKind::Soloist;
    CHECK(changed != *res);
    changed = *res;
    changed.pop_back();
    CHECK(changed != *res);
    auto const owningRes = normalizeCredits(*res);
    REQUIRE(owningRes);
    CHECK(*owningRes == *res);
  }

  TEST_CASE("Credits - fixed kind tokens reject unknown enum values and spellings", "[library][unit][credits]")
  {
    auto const tokens = std::array{"conductor", "ensemble", "soloist", "performer"};

    for (std::size_t index = 0; index < tokens.size(); ++index)
    {
      auto const kind = static_cast<CreditKind>(index);
      CHECK(isValidCreditKind(kind));
      CHECK(creditKindToken(kind) == tokens[index]);
      CHECK(parseCreditKind(tokens[index]) == kind);
    }

    auto const invalid = static_cast<CreditKind>(255);
    CHECK_FALSE(isValidCreditKind(invalid));
    CHECK(creditKindToken(invalid).empty());
    CHECK_FALSE(parseCreditKind("Soloist"));
    CHECK_FALSE(parseCreditKind("musician"));
    CHECK_FALSE(parseCreditKind(""));
    auto const res = normalizeList({{.name = "Name", .kind = invalid}});
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    auto const owning = std::vector<Credit>{{.name = "Name", .kind = invalid}};
    CHECK_FALSE(normalizeCredits(owning));
  }
} // namespace ao::library::test

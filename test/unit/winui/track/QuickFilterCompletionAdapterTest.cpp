// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/track/QuickFilterCompletionAdapter.h>

#include "test/unit/PresentationTextCatalogTestSupport.h"
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ao::winui::test
{
  TEST_CASE("quickFilterUtf8Cursor maps UTF-16 caret boundaries to UTF-8 bytes", "[winui][unit][quick-filter]")
  {
    auto const text = std::string{"A\xF0\x9F\x8E\xB5\xE5\xAE\x87"};

    CHECK(quickFilterUtf8Cursor(text, 0) == 0);
    CHECK(quickFilterUtf8Cursor(text, 1) == 1);
    CHECK(quickFilterUtf8Cursor(text, 3) == 5);
    CHECK(quickFilterUtf8Cursor(text, 4) == 8);
  }

  TEST_CASE("quickFilterUtf8Cursor rejects invalid and non-boundary cursors", "[winui][unit][quick-filter]")
  {
    CHECK_FALSE(quickFilterUtf8Cursor("\xF0\x9F\x8E\xB5", 1));
    CHECK_FALSE(quickFilterUtf8Cursor("A", 2));
    CHECK_FALSE(quickFilterUtf8Cursor("\xC3", 1));
  }

  TEST_CASE("quickFilterUtf16Range maps quoted non-ASCII replacement spans", "[winui][unit][quick-filter]")
  {
    auto const text = std::string{"$artist = \"\xE5\xAE\x87\xE5\xA4\x9A\" and $year"};
    auto const begin = text.find("\xE5\xAE\x87");
    auto const end = text.find('"', begin);

    CHECK(quickFilterUtf16Range(text, begin, end) == QuickFilterUtf16Range{.begin = 11, .end = 13});
  }

  TEST_CASE("quickFilterUtf16Range rejects stale or malformed spans", "[winui][unit][quick-filter]")
  {
    CHECK_FALSE(quickFilterUtf16Range("Aimer", 4, 2));
    CHECK_FALSE(quickFilterUtf16Range("Aimer", 0, 6));
    CHECK_FALSE(quickFilterUtf16Range("\xE5\xAE\x87", 1, 3));
    CHECK_FALSE(quickFilterUtf16Range("\xE5", 0, 1));
  }

  TEST_CASE("quickFilterSuggestionRows preserves semantic item identity and localized detail",
            "[winui][unit][quick-filter]")
  {
    auto const result =
      rt::CompletionResult{.replaceBegin = 0,
                           .replaceEnd = 5,
                           .items = {{.displayText = "\xE5\xAE\x87\xE5\xA4\x9A\xE7\x94\xB0\xE5\x85\x89",
                                      .insertText = "\"\xE5\xAE\x87\xE5\xA4\x9A\xE7\x94\xB0\xE5\x85\x89\"",
                                      .detail = rt::CompletionDetail::makeUsageFrequency(42),
                                      .rank = 7}}};

    auto const rows = quickFilterSuggestionRows(result, ao::test::englishPresentationTextCatalog());

    REQUIRE(rows.size() == 1);
    CHECK(rows[0].displayText == "\xE5\xAE\x87\xE5\xA4\x9A\xE7\x94\xB0\xE5\x85\x89");
    CHECK(rows[0].insertText == "\"\xE5\xAE\x87\xE5\xA4\x9A\xE7\x94\xB0\xE5\x85\x89\"");
    CHECK(rows[0].detailText == "42");
    CHECK(rows[0].detailKind == rt::CompletionDetailKind::Frequency);
    CHECK(rows[0].rank == 7);
  }

  TEST_CASE("quickFilterSuggestionContinuesEditing distinguishes incomplete expression tokens",
            "[winui][unit][quick-filter][regression]")
  {
    CHECK(
      quickFilterSuggestionContinuesEditing({.insertText = "$artist", .detailKind = rt::CompletionDetailKind::Field}));
    CHECK(quickFilterSuggestionContinuesEditing(
      {.insertText = " and ", .detailKind = rt::CompletionDetailKind::LogicalOperator}));
    CHECK(
      quickFilterSuggestionContinuesEditing({.insertText = " = ", .detailKind = rt::CompletionDetailKind::Operator}));
    CHECK_FALSE(
      quickFilterSuggestionContinuesEditing({.insertText = "?", .detailKind = rt::CompletionDetailKind::Operator}));
    CHECK_FALSE(quickFilterSuggestionContinuesEditing(
      {.insertText = "\"Aimer\"", .detailKind = rt::CompletionDetailKind::Frequency}));
  }
} // namespace ao::winui::test

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/TuiTextFieldModel.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace ao::tui::test
{
  namespace
  {
    constexpr auto kFamily = std::string_view{"👨‍👩‍👧‍👦"};
    constexpr auto kJapanFlag = std::string_view{"🇯🇵"};
    constexpr auto kCombiningAcute = std::string_view{"́"};
  } // namespace

  TEST_CASE("TuiTextFieldModel - loads a value with the cursor at its end", "[tui][unit][editor]")
  {
    auto field = TuiTextFieldModel{std::string{"Blue"}};

    CHECK(field.value() == "Blue");
    CHECK(field.cursor() == 4);
    CHECK_FALSE(field.empty());

    SECTION("Reloading replaces the value and parks the cursor again")
    {
      field.reset("Kind of Blue");

      CHECK(field.value() == "Kind of Blue");
      CHECK(field.cursor() == 12);
    }

    SECTION("Malformed bytes load as an empty value instead of poisoning later edits")
    {
      field.reset(std::string{"\x80"});

      CHECK(field.value().empty());
      CHECK(field.cursor() == 0);
      CHECK(field.empty());
    }
  }

  TEST_CASE("TuiTextFieldModel - moves and edits on whole grapheme clusters", "[tui][unit][editor]")
  {
    auto field = TuiTextFieldModel{std::string{"a"} + std::string{kFamily} + "b"};

    SECTION("A cursor step crosses a joined sequence in one move")
    {
      REQUIRE(field.moveToBegin());
      CHECK(field.cursor() == 0);
      CHECK(field.moveRight());
      CHECK(field.cursor() == 1);
      CHECK(field.moveRight());
      CHECK(field.cursor() == 1 + kFamily.size());
      CHECK(field.moveLeft());
      CHECK(field.cursor() == 1);
    }

    SECTION("Backspace removes the joined sequence whole")
    {
      REQUIRE(field.moveLeft());
      REQUIRE(field.cursor() == 1 + kFamily.size());

      CHECK(field.backspace());
      CHECK(field.value() == "ab");
      CHECK(field.cursor() == 1);
    }

    SECTION("Delete removes the cluster ahead of the cursor")
    {
      REQUIRE(field.moveToBegin());
      REQUIRE(field.moveRight());

      CHECK(field.deleteForward());
      CHECK(field.value() == "ab");
      CHECK(field.cursor() == 1);
    }

    SECTION("Regional indicators pair rather than splitting")
    {
      field.reset(std::string{kJapanFlag});

      CHECK(field.backspace());
      CHECK(field.value().empty());
      CHECK(field.cursor() == 0);
    }
  }

  TEST_CASE("TuiTextFieldModel - inserts at the cursor and reports acceptance", "[tui][unit][editor]")
  {
    auto field = TuiTextFieldModel{std::string{"Blue"}};

    SECTION("Interior insertion keeps the cursor after the typed text")
    {
      REQUIRE(field.moveToBegin());

      CHECK(field.insert("A "));
      CHECK(field.value() == "A Blue");
      CHECK(field.cursor() == 2);
    }

    SECTION("Text that joins the following cluster leaves the cursor on a boundary")
    {
      field.reset(std::string{kCombiningAcute});
      REQUIRE(field.moveToBegin());

      CHECK(field.insert("a"));
      CHECK(field.value() == std::string{"a"} + std::string{kCombiningAcute});
      // The typed letter and the following mark are now one cluster, so the
      // cursor settles after it rather than inside it.
      CHECK(field.cursor() == field.value().size());
      CHECK(field.moveLeft());
      CHECK(field.cursor() == 0);
    }

    SECTION("An empty insertion is not an edit")
    {
      CHECK_FALSE(field.insert(""));
      CHECK(field.value() == "Blue");
    }
  }

  TEST_CASE("TuiTextFieldModel - refuses input a single-line field cannot hold", "[tui][unit][editor]")
  {
    auto field = TuiTextFieldModel{std::string{"Blue"}};

    auto const checkRejected = [&field](std::string_view const text)
    {
      CHECK_FALSE(field.insert(text));
      CHECK(field.value() == "Blue");
      CHECK(field.cursor() == 4);
    };

    // A rejected paste must not become a partially applied edit: the editor
    // reads the returned false as "no Apply intent", so any surviving byte
    // would be an edit nobody asked for.
    checkRejected("Kind of\nBlue");
    checkRejected("Kind of\rBlue");
    checkRejected("Kind\tof Blue");
    checkRejected("\x1b[31mBlue");
    checkRejected("Blue\x7f");
    checkRejected("Blue\u009b");
    checkRejected(std::string{"Bl"} + static_cast<char>(0x80) + "ue");
  }

  TEST_CASE("TuiTextFieldModel - refuses Unicode line and paragraph separators", "[tui][unit][editor]")
  {
    auto model = TuiTextFieldModel{"Kind of Blue"};

    // U+2028 and U+2029 break a line as surely as U+000A, so a single-line
    // value refuses them on the same grounds and leaves itself untouched.
    CHECK_FALSE(model.insert("\u2028"));
    CHECK_FALSE(model.insert("So\u2028What"));
    CHECK_FALSE(model.insert("So\u2029What"));
    CHECK(model.value() == "Kind of Blue");
    CHECK(model.cursor() == model.value().size());

    // U+2027 and U+202A share bytes with them and are ordinary characters.
    CHECK(model.insert("\u2027"));
    CHECK(model.value() == "Kind of Blue\u2027");
  }

  TEST_CASE("TuiTextFieldModel - reports no change at the value's edges", "[tui][unit][editor]")
  {
    auto field = TuiTextFieldModel{};

    SECTION("An empty value has nothing to move across or delete")
    {
      CHECK_FALSE(field.moveLeft());
      CHECK_FALSE(field.moveRight());
      CHECK_FALSE(field.moveToBegin());
      CHECK_FALSE(field.moveToEnd());
      CHECK_FALSE(field.backspace());
      CHECK_FALSE(field.deleteForward());
      CHECK(field.empty());
    }

    SECTION("Backspace at the start and Delete at the end are not edits")
    {
      field.reset("Blue");

      CHECK_FALSE(field.deleteForward());
      REQUIRE(field.moveToBegin());
      CHECK_FALSE(field.backspace());
      CHECK_FALSE(field.moveToBegin());
      CHECK(field.value() == "Blue");
    }
  }

  TEST_CASE("TuiTextFieldModel - replaces ranges with checked invariants", "[tui][unit][editor]")
  {
    auto field = TuiTextFieldModel{std::string{"Blue"}};

    SECTION("Replaces entire text and parks cursor at end")
    {
      CHECK(field.replaceRange(0, 4, "Kind of Blue"));
      CHECK(field.value() == "Kind of Blue");
      CHECK(field.cursor() == 12);
    }

    SECTION("Replaces range with empty string (deletion)")
    {
      field.reset("Kind of Blue");
      CHECK(field.replaceRange(4, 7, ""));
      CHECK(field.value() == "Kind Blue");
      CHECK(field.cursor() == 4);
    }

    SECTION("Accepts same-value replacement")
    {
      CHECK(field.replaceRange(0, 4, "Blue"));
      CHECK(field.value() == "Blue");
      CHECK(field.cursor() == 4);
    }

    SECTION("Rejects invalid range boundaries")
    {
      CHECK_FALSE(field.replaceRange(3, 2, "X"));
      CHECK_FALSE(field.replaceRange(0, 5, "X"));
      CHECK(field.value() == "Blue");
      CHECK(field.cursor() == 4);
    }

    SECTION("Rejects non-grapheme boundaries atomically")
    {
      field.reset(std::string{kFamily});
      REQUIRE(field.cursor() == kFamily.size());

      // Cut into the family emoji cluster
      CHECK_FALSE(field.replaceRange(0, 4, "X"));
      CHECK_FALSE(field.replaceRange(2, kFamily.size(), "X"));
      CHECK(field.value() == kFamily);
      CHECK(field.cursor() == kFamily.size());
    }

    SECTION("Rejects control characters and invalid UTF-8")
    {
      CHECK_FALSE(field.replaceRange(0, 4, "Kind of\nBlue"));
      CHECK_FALSE(field.replaceRange(0, 4, "Kind\tof Blue"));
      CHECK_FALSE(field.replaceRange(0, 4, "\x1b[31mBlue"));
      CHECK_FALSE(field.replaceRange(0, 4, std::string{"Bl"} + static_cast<char>(0x80) + "ue"));
      CHECK(field.value() == "Blue");
      CHECK(field.cursor() == 4);
    }

    SECTION("Correctly settles caret with multibyte and emoji replacements")
    {
      CHECK(field.replaceRange(0, 4, std::string{kJapanFlag}));
      CHECK(field.value() == kJapanFlag);
      CHECK(field.cursor() == kJapanFlag.size());
    }
  }
} // namespace ao::tui::test

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkAccelTranslator.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>

#include <string>

namespace ao::gtk::test
{
  namespace
  {
    uimodel::KeyChord chord(std::string const& text)
    {
      auto const optChord = uimodel::KeyChord::parse(text);
      REQUIRE(optChord.has_value());
      return *optChord;
    }
  }

  TEST_CASE("GtkAccelTranslator maps neutral chords to GTK accelerators", "[gtk][unit][app][accel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    SECTION("ctrl letter")
    {
      auto const optAccel = toGtkAccel(chord("Ctrl+P"));
      REQUIRE(optAccel.has_value());
      // GTK lowercases the letter name in the canonical accel string.
      CHECK(*optAccel == "<Control>p");
    }

    SECTION("named navigation key")
    {
      auto const optAccel = toGtkAccel(chord("Ctrl+Right"));
      REQUIRE(optAccel.has_value());
      CHECK(*optAccel == "<Control>Right");
    }

    SECTION("media key without modifier")
    {
      auto const optAccel = toGtkAccel(chord("Media:Play"));
      REQUIRE(optAccel.has_value());
      CHECK(*optAccel == "AudioPlay");
    }

    SECTION("invalid key token yields nullopt")
    {
      CHECK(toGtkAccel(chord("NotARealKey")).has_value() == false);
    }
  }

  TEST_CASE("GtkAccelTranslator parses GTK accelerators back to neutral chords", "[gtk][unit][app][accel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    CHECK(fromGtkAccel("<Primary>p") == chord("Ctrl+P"));
    CHECK(fromGtkAccel("<Control>Right") == chord("Ctrl+Right"));
    CHECK(fromGtkAccel("AudioNext") == chord("Media:Next"));
    CHECK(fromGtkAccel("not-an-accel").has_value() == false);
  }

  TEST_CASE("GtkAccelTranslator converts live key presses to chords", "[gtk][unit][app][accel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    SECTION("letter with control modifier")
    {
      auto const optResult = fromGtkKeyval(GDK_KEY_p, Gdk::ModifierType::CONTROL_MASK);
      REQUIRE(optResult.has_value());
      CHECK(*optResult == chord("Ctrl+P"));
    }

    SECTION("plain navigation key with no modifier")
    {
      auto const optResult = fromGtkKeyval(GDK_KEY_Right, Gdk::ModifierType{});
      REQUIRE(optResult.has_value());
      CHECK(*optResult == chord("Right"));
    }

    SECTION("non-accelerator lock bits are dropped")
    {
      auto const optResult = fromGtkKeyval(GDK_KEY_a, Gdk::ModifierType::SHIFT_MASK | Gdk::ModifierType::LOCK_MASK);
      REQUIRE(optResult.has_value());
      CHECK(*optResult == chord("Shift+A"));
    }

    SECTION("standalone modifier keys are rejected so capture keeps waiting")
    {
      CHECK(fromGtkKeyval(GDK_KEY_Control_L, Gdk::ModifierType::CONTROL_MASK).has_value() == false);
      CHECK(fromGtkKeyval(GDK_KEY_Shift_R, Gdk::ModifierType::SHIFT_MASK).has_value() == false);
      CHECK(fromGtkKeyval(GDK_KEY_Alt_L, Gdk::ModifierType::ALT_MASK).has_value() == false);
    }
  }

  TEST_CASE("GtkAccelTranslator round-trips the default keymap", "[gtk][unit][app][accel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    for (auto const& [actionId, chords] : uimodel::defaultKeymap())
    {
      for (auto const& original : chords)
      {
        auto const optAccel = toGtkAccel(original);
        REQUIRE(optAccel.has_value());

        auto const optParsed = fromGtkAccel(*optAccel);
        REQUIRE(optParsed.has_value());
        CHECK(*optParsed == original);
      }
    }
  }
} // namespace ao::gtk::test

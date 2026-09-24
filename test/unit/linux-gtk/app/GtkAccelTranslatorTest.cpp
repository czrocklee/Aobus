// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkAccelTranslator.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::test
{
  namespace
  {
    uimodel::KeyChord chord(std::string const& text)
    {
      auto const optChord = uimodel::KeyChord::parse(text);
      REQUIRE(optChord);
      return *optChord;
    }
  }

  TEST_CASE("GtkAccelTranslator - maps neutral chords to GTK accelerators", "[gtk][unit][app][accel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    SECTION("ctrl letter")
    {
      auto const optAccel = toGtkAccel(chord("Ctrl+P"));
      REQUIRE(optAccel);
      // GTK lowercases the letter name in the canonical accel string.
      CHECK(*optAccel == "<Control>p");
    }

    SECTION("named navigation key")
    {
      auto const optAccel = toGtkAccel(chord("Ctrl+Right"));
      REQUIRE(optAccel);
      CHECK(*optAccel == "<Control>Right");
    }

    SECTION("media key without modifier")
    {
      auto const optAccel = toGtkAccel(chord("Media:Play"));
      REQUIRE(optAccel);
      CHECK(*optAccel == "AudioPlay");
    }

    SECTION("invalid key token yields nullopt")
    {
      CHECK(toGtkAccel(chord("NotARealKey")).has_value() == false);
    }

    SECTION("every exceptional neutral spelling maps to its GTK name")
    {
      constexpr auto kAliases = std::to_array<std::pair<std::string_view, std::string_view>>({
        {"Space", "space"},
        {"Enter", "Return"},
        {"Backspace", "BackSpace"},
        {"PageUp", "Page_Up"},
        {"PageDown", "Page_Down"},
        {"Media:Play", "AudioPlay"},
        {"Media:Pause", "AudioPause"},
        {"Media:Stop", "AudioStop"},
        {"Media:Next", "AudioNext"},
        {"Media:Prev", "AudioPrev"},
      });

      for (auto const& [token, gtkName] : kAliases)
      {
        auto const optAccel = toGtkAccel(chord(std::string{token}));
        REQUIRE(optAccel);
        CHECK(*optAccel == gtkName);
      }
    }
  }

  TEST_CASE("GtkAccelTranslator - parses GTK accelerators back to neutral chords", "[gtk][unit][app][accel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    CHECK(fromGtkAccel("<Primary>p") == chord("Ctrl+P"));
    CHECK(fromGtkAccel("<Control>Right") == chord("Ctrl+Right"));
    CHECK(fromGtkAccel("AudioNext") == chord("Media:Next"));
    CHECK(fromGtkAccel("not-an-accel").has_value() == false);

    constexpr auto kAliases = std::to_array<std::pair<std::string_view, std::string_view>>({
      {"space", "Space"},
      {"Return", "Enter"},
      {"BackSpace", "Backspace"},
      {"Page_Up", "PageUp"},
      {"Page_Down", "PageDown"},
      {"AudioPlay", "Media:Play"},
      {"AudioPause", "Media:Pause"},
      {"AudioStop", "Media:Stop"},
      {"AudioNext", "Media:Next"},
      {"AudioPrev", "Media:Prev"},
    });

    for (auto const& [gtkName, token] : kAliases)
    {
      CHECK(fromGtkAccel(std::string{gtkName}) == chord(std::string{token}));
    }
  }

  TEST_CASE("GtkAccelTranslator - converts live key presses to chords", "[gtk][unit][app][accel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    SECTION("letter with control modifier")
    {
      auto const optResult = fromGtkKeyval(GDK_KEY_p, Gdk::ModifierType::CONTROL_MASK);
      REQUIRE(optResult);
      CHECK(*optResult == chord("Ctrl+P"));
    }

    SECTION("plain navigation key with no modifier")
    {
      auto const optResult = fromGtkKeyval(GDK_KEY_Right, Gdk::ModifierType{});
      REQUIRE(optResult);
      CHECK(*optResult == chord("Right"));
    }

    SECTION("non-accelerator lock bits are dropped")
    {
      auto const optResult = fromGtkKeyval(GDK_KEY_a, Gdk::ModifierType::SHIFT_MASK | Gdk::ModifierType::LOCK_MASK);
      REQUIRE(optResult);
      CHECK(*optResult == chord("Shift+A"));
    }

    SECTION("standalone modifier keys are rejected so capture keeps waiting")
    {
      constexpr auto kModifierKeys = std::to_array<std::pair<guint, Gdk::ModifierType>>({
        {GDK_KEY_Control_L, Gdk::ModifierType::CONTROL_MASK},
        {GDK_KEY_Control_R, Gdk::ModifierType::CONTROL_MASK},
        {GDK_KEY_Shift_L, Gdk::ModifierType::SHIFT_MASK},
        {GDK_KEY_Shift_R, Gdk::ModifierType::SHIFT_MASK},
        {GDK_KEY_Shift_Lock, Gdk::ModifierType::LOCK_MASK},
        {GDK_KEY_Caps_Lock, Gdk::ModifierType::LOCK_MASK},
        {GDK_KEY_Alt_L, Gdk::ModifierType::ALT_MASK},
        {GDK_KEY_Alt_R, Gdk::ModifierType::ALT_MASK},
        {GDK_KEY_Meta_L, Gdk::ModifierType::META_MASK},
        {GDK_KEY_Meta_R, Gdk::ModifierType::META_MASK},
        {GDK_KEY_Super_L, Gdk::ModifierType::SUPER_MASK},
        {GDK_KEY_Super_R, Gdk::ModifierType::SUPER_MASK},
        {GDK_KEY_Hyper_L, Gdk::ModifierType::HYPER_MASK},
        {GDK_KEY_Hyper_R, Gdk::ModifierType::HYPER_MASK},
        {GDK_KEY_ISO_Level3_Shift, Gdk::ModifierType{}},
        {GDK_KEY_Num_Lock, Gdk::ModifierType{}},
      });

      for (auto const& [keyval, state] : kModifierKeys)
      {
        CHECK_FALSE(fromGtkKeyval(keyval, state).has_value());
      }
    }
  }

  TEST_CASE("GtkAccelTranslator - round-trips the default keymap", "[gtk][unit][app][accel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    for (auto const& [actionId, chords] : uimodel::defaultKeymap())
    {
      for (auto const& original : chords)
      {
        auto const optAccel = toGtkAccel(original);
        REQUIRE(optAccel);

        auto const optParsed = fromGtkAccel(*optAccel);
        REQUIRE(optParsed);
        CHECK(*optParsed == original);
      }
    }
  }
} // namespace ao::gtk::test

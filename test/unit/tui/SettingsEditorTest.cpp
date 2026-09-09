// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/SettingsEditor.h"

#include "Preferences.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "tui/Keymap.h"
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    struct SettingsFixture final
    {
      i18n::MessageCatalog catalog{ao::test::englishMessageCatalog()};
      Preferences preferences;
      uimodel::KeymapModel keymap{defaultKeymap()};
      bool fail = false;
      SettingsEditor editor{
        catalog,
        preferences,
        keymap,
        SettingsEditor::Outputs{.applyPreferences = [&](Preferences const& candidate) -> Result<>
                                {
                                  if (fail)
                                  {
                                    return makeError(Error::Code::InvalidInput, "save denied");
                                  }

                                  preferences = candidate;

                                  if (!candidate.language.empty())
                                  {
                                    catalog = ao::test::requireValue(i18n::MessageCatalog::create(candidate.language));
                                  }

                                  return {};
                                },
                                .applyKeymap = [&](uimodel::KeymapModel const& candidate) -> Result<>
                                {
                                  if (fail)
                                  {
                                    return makeError(Error::Code::InvalidInput, "save denied");
                                  }

                                  keymap = candidate;
                                  return {};
                                },
                                .coverMode = [] { return std::string{"off"}; }}};

      SettingsFixture()
      {
        // Keyboard-editing scenarios start with an explicitly unbound action.
        keymap.applyOverrides({{"tui.shell.openSettings", {}}});
      }

      void page(SettingsPage target)
      {
        while (editor.page() != target)
        {
          REQUIRE(editor.tryHandleEvent(ftxui::Event::Tab));
        }
      }
      std::string render(std::int32_t const columns = 80, std::int32_t const rows = 24) const
      {
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(columns), ftxui::Dimension::Fixed(rows));
        ftxui::Render(screen, editor.renderModal(columns, rows));
        return screen.ToString();
      }
    };
  } // namespace

  TEST_CASE("SettingsEditor - language choice applies on confirmation and refreshes its own labels",
            "[tui][unit][settings][localization]")
  {
    auto fixture = SettingsFixture{};
    fixture.editor.open();
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::ArrowDown));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::ArrowDown));
    CHECK(fixture.preferences.language.empty());
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    CHECK(fixture.preferences.language == "de");
    CHECK(fixture.render().contains("Einstellungen"));
    CHECK(fixture.render().contains("Sprache"));
  }

  TEST_CASE("SettingsEditor - language chooser displays every catalog locale by its native name",
            "[tui][unit][settings][localization]")
  {
    auto fixture = SettingsFixture{};

    for (auto const& locale : i18n::availableCatalogLocales())
    {
      INFO(locale.tag);
      fixture.preferences.language = locale.tag;
      fixture.editor.open();
      REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
      CHECK(fixture.render().contains(locale.selfName));
      REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
      CHECK(fixture.preferences.language == locale.tag);
    }
  }

  TEST_CASE("SettingsEditor - cover renderer cycles in both directions and wraps", "[tui][unit][settings]")
  {
    auto fixture = SettingsFixture{};
    fixture.editor.open();
    fixture.page(SettingsPage::Appearance);
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::ArrowDown));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::ArrowDown));

    for (auto const* mode : {"kitty", "blocks", "off", "auto"})
    {
      REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::ArrowRight));
      CHECK(fixture.preferences.coverArtMode == mode);
    }

    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::ArrowLeft));
    CHECK(fixture.preferences.coverArtMode == "off");
  }

  TEST_CASE("SettingsEditor - failed preference save preserves applied values until retry", "[tui][unit][settings]")
  {
    auto fixture = SettingsFixture{};
    fixture.editor.open();
    fixture.page(SettingsPage::Appearance);
    fixture.fail = true;
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    CHECK(fixture.preferences.dimBackdrop);
    CHECK(fixture.render().contains("save denied"));
    fixture.fail = false;
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::CtrlR));
    CHECK_FALSE(fixture.preferences.dimBackdrop);
    CHECK_FALSE(fixture.render().contains("save denied"));
  }

  TEST_CASE("SettingsEditor - save failures show localized context and retain the diagnostic",
            "[tui][unit][settings][localization]")
  {
    for (auto const page : {SettingsPage::Appearance, SettingsPage::Keyboard})
    {
      auto fixture = SettingsFixture{};
      fixture.catalog = ao::test::messageCatalog("zh-Hans");
      fixture.fail = true;
      fixture.editor.open();
      fixture.page(page);
      REQUIRE(fixture.editor.tryHandleEvent(page == SettingsPage::Keyboard ? ftxui::Event::Character("r")
                                                                           : ftxui::Event::Return));
      auto const rendered = fixture.render();
      CHECK(rendered.contains("无法保存设置。"));
      CHECK(rendered.contains("save denied"));
      CHECK(rendered.contains("Ctrl+R"));
    }
  }

  TEST_CASE("SettingsEditor - successful retry leaves the close confirmation and restores editing",
            "[tui][regression][settings]")
  {
    auto fixture = SettingsFixture{};
    fixture.editor.open();
    fixture.page(SettingsPage::Appearance);
    fixture.fail = true;
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Escape));
    REQUIRE(fixture.render().contains("Close and discard"));
    fixture.fail = false;
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::CtrlR));
    CHECK_FALSE(fixture.preferences.dimBackdrop);
    CHECK_FALSE(fixture.render().contains("Close and discard"));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Tab));
    CHECK(fixture.editor.page() == SettingsPage::Interaction);
  }

  TEST_CASE("SettingsEditor - discarding a failed candidate preserves effective preferences", "[tui][unit][settings]")
  {
    auto fixture = SettingsFixture{};
    fixture.editor.open();
    fixture.page(SettingsPage::Appearance);
    fixture.fail = true;
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::CtrlG));
    CHECK(fixture.preferences.dimBackdrop);
    CHECK_FALSE(fixture.render().contains("save denied"));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(fixture.editor.isActive());
  }

  TEST_CASE("SettingsEditor - confirming close discards a failed keymap candidate", "[tui][unit][settings]")
  {
    auto fixture = SettingsFixture{};
    fixture.editor.open();
    fixture.page(SettingsPage::Keyboard);
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Insert));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Character("F12")));
    fixture.fail = true;
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Escape));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    CHECK_FALSE(fixture.editor.isActive());
    CHECK(fixture.keymap.chordsFor("tui.shell.openSettings").empty());
    fixture.editor.open();
    CHECK_FALSE(fixture.render().contains("Close and discard"));
  }

  TEST_CASE("SettingsEditor - resetting an action restores defaults and selected binding",
            "[tui][unit][settings][keymap]")
  {
    auto fixture = SettingsFixture{};
    fixture.editor.open();
    fixture.page(SettingsPage::Keyboard);
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Insert));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Character("F12")));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Character("r")));
    CHECK(fixture.keymap.chordsFor("tui.shell.openSettings") == std::vector{*uimodel::KeyChord::parse(",")});
    CHECK(fixture.render().contains("[,]"));
  }

  TEST_CASE("SettingsEditor - replacing a chord removes the old binding and retains its siblings",
            "[tui][regression][settings][keymap]")
  {
    auto fixture = SettingsFixture{};
    REQUIRE(fixture.keymap.tryBind("tui.shell.openSettings", *uimodel::KeyChord::parse("F12")));
    REQUIRE(fixture.keymap.tryBind("tui.shell.openSettings", *uimodel::KeyChord::parse("F11")));
    fixture.editor.open();
    fixture.page(SettingsPage::Keyboard);
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Backspace));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Character("0")));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    auto const chords = fixture.keymap.chordsFor("tui.shell.openSettings");
    REQUIRE(chords.size() == 2);
    CHECK(std::ranges::contains(chords, *uimodel::KeyChord::parse("F10")));
    CHECK(std::ranges::contains(chords, *uimodel::KeyChord::parse("F11")));
    CHECK_FALSE(std::ranges::contains(chords, *uimodel::KeyChord::parse("F12")));
  }

  TEST_CASE("SettingsEditor - adding and removing a binding preserves the other chords",
            "[tui][unit][settings][keymap]")
  {
    auto fixture = SettingsFixture{};
    fixture.editor.open();
    fixture.page(SettingsPage::Keyboard);
    // The fixture explicitly unbinds Settings, the first row.
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Insert));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Character("F12")));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    CHECK(fixture.keymap.chordsFor("tui.shell.openSettings") == std::vector{*uimodel::KeyChord::parse("F12")});
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Insert));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Character("F11")));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::ArrowRight));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Delete));
    CHECK(fixture.render().contains("[F12]"));
    CHECK(fixture.keymap.chordsFor("tui.shell.openSettings") == std::vector{*uimodel::KeyChord::parse("F12")});
  }

  TEST_CASE("SettingsEditor - conflicting chord is rejected and failed save keeps live bindings unchanged",
            "[tui][unit][settings][keymap]")
  {
    auto fixture = SettingsFixture{};
    fixture.editor.open();
    fixture.page(SettingsPage::Keyboard);
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Insert));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Character("Q")));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    CHECK(fixture.keymap.chordsFor("tui.shell.openSettings").empty());
    CHECK(fixture.render().contains("Already assigned to quit"));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Escape));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Insert));
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Character("F12")));
    fixture.fail = true;
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Return));
    CHECK(fixture.keymap.chordsFor("tui.shell.openSettings").empty());
    CHECK(fixture.render().contains("F12"));
    fixture.fail = false;
    REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::CtrlR));
    CHECK(fixture.keymap.chordsFor("tui.shell.openSettings") == std::vector{*uimodel::KeyChord::parse("F12")});
  }

  TEST_CASE("SettingsEditor - all maintained languages and pseudo keep page controls visible",
            "[tui][unit][settings][localization]")
  {
    auto fixture = SettingsFixture{};

    auto locales = std::vector<std::string_view>{"qps-ploc"};

    for (auto const& locale : i18n::availableCatalogLocales())
    {
      locales.push_back(locale.tag);
    }

    for (auto const locale : locales)
    {
      fixture.catalog = ao::test::messageCatalog(locale);
      fixture.editor.open();

      for (std::int32_t index = 0; index < 4; ++index)
      {
        INFO(locale);
        INFO(index);
        auto const rendered = fixture.render();
        CHECK(rendered.contains("Esc"));
        CHECK(rendered.contains("Tab"));
        REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Tab));
      }
    }
  }

  TEST_CASE("SettingsEditor - all pages keep close and page controls visible in a small terminal",
            "[tui][unit][settings][render]")
  {
    for (auto const columns : {36, 80})
    {
      INFO(columns);
      auto fixture = SettingsFixture{};
      fixture.editor.open();

      for (auto const page : std::array{
             SettingsPage::General, SettingsPage::Appearance, SettingsPage::Interaction, SettingsPage::Keyboard})
      {
        CHECK(fixture.editor.page() == page);
        auto const rendered = fixture.render(columns, 24);
        CHECK(rendered.contains("Esc"));
        CHECK(rendered.contains("Tab"));
        REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::Tab));
      }

      CHECK(fixture.editor.page() == SettingsPage::General);
      REQUIRE(fixture.editor.tryHandleEvent(ftxui::Event::TabReverse));
      CHECK(fixture.editor.page() == SettingsPage::Keyboard);
    }
  }
} // namespace ao::tui::test

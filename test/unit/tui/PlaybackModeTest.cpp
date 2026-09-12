// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/PlaybackMode.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/MouseBindings.h"
#include "tui/PanelResize.h"
#include "tui/PlaybackPanel.h"
#include "tui/ShellInteractionModel.h"
#include "tui/StatusBar.h"
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include <array>
#include <chrono>
#include <list>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui::test
{
  TEST_CASE("PlaybackMode - padded codes remain stable across states and languages", "[tui][unit][playback-mode]")
  {
    for (std::string_view const locale : {"en", "zh-Hans", "zh-Hant", "ja", "de", "es", "fr"})
    {
      auto const catalog = ao::test::messageCatalog(locale);

      for (auto const width : {48, 140})
      {
        auto firstBox = kEmptyMouseBox;

        for (auto const& [choice, code] : std::array{
               std::pair{PlaybackModeChoice{}, "SEQ-"},
               std::pair{PlaybackModeChoice{.repeat = rt::RepeatMode::All}, "SEQ*"},
               std::pair{PlaybackModeChoice{.shuffle = rt::ShuffleMode::On}, "SHF-"},
               std::pair{PlaybackModeChoice{.shuffle = rt::ShuffleMode::On, .repeat = rt::RepeatMode::All}, "SHF*"},
               std::pair{PlaybackModeChoice{.repeat = rt::RepeatMode::One}, "SEQ1"},
               std::pair{PlaybackModeChoice{.shuffle = rt::ShuffleMode::On, .repeat = rt::RepeatMode::One}, "SHF1"}})
        {
          CAPTURE(locale, width, code);
          auto succession = rt::PlaybackSuccessionSnapshot{.shuffle = choice.shuffle, .repeat = choice.repeat};
          auto box = kEmptyMouseBox;
          auto output = uimodel::OutputDeviceViewState{.outputBackendSummary = "PW", .hasActiveOutputDevice = true};
          auto outputBox = kEmptyMouseBox;
          auto view = PlaybackBarViewState{.succession = &succession,
                                           .outputView = &output,
                                           .outputDeviceBox = &outputBox,
                                           .playbackModeBox = &box,
                                           .terminalColumns = width};
          auto const normal = renderElement(playbackBar(catalog, view), width, 1);
          auto const normalBox = box;
          view.playbackModeHovered = true;
          auto const hovered = renderElement(playbackBar(catalog, view), width, 1);
          REQUIRE_FALSE(box.IsEmpty());
          CHECK(box.x_min >= 0);
          CHECK(box.x_max < width);
          CHECK(box.x_max - box.x_min + 1 == 6);
          CHECK(box.x_min == normalBox.x_min);
          CHECK(box.x_max == normalBox.x_max);
          CHECK(hovered.text == normal.text);
          CHECK(hovered.screen.PixelAt(box.x_min, box.y_min).character.empty());
          CHECK(hovered.screen.PixelAt(box.x_max, box.y_min).character.empty());
          auto buttonText = std::string{};

          for (auto column = box.x_min; column <= box.x_max; ++column)
          {
            buttonText += hovered.screen.PixelAt(column, box.y_min).character;
            checkInteractiveSurface(hovered.screen.PixelAt(column, box.y_min));
            CHECK(normal.screen.PixelAt(column, box.y_min).background_color == ftxui::Color::Default);
          }

          std::erase(buttonText, ' ');
          CHECK(buttonText == code);
          REQUIRE_FALSE(outputBox.IsEmpty());
          CHECK(box.x_max + 1 == outputBox.x_min);
          CHECK(outputBox.x_max < width);
          CHECK(hovered.text.contains(std::string{code} + "  PW"));
          CHECK(hovered.screen.PixelAt(outputBox.x_min, outputBox.y_min).background_color == ftxui::Color::Default);
          view.playbackModeHovered = false;
          view.outputDeviceHovered = true;
          auto const outputHovered = renderElement(playbackBar(catalog, view), width, 1);
          CHECK(outputHovered.text == normal.text);
          checkInteractiveSurface(outputHovered.screen.PixelAt(outputBox.x_min, outputBox.y_min));
          CHECK(outputHovered.screen.PixelAt(box.x_max, box.y_min).background_color == ftxui::Color::Default);

          if (firstBox.IsEmpty())
          {
            firstBox = box;
          }

          CHECK(box.x_min == firstBox.x_min);
          CHECK(box.x_max == firstBox.x_max);
        }
      }
    }
  }

  TEST_CASE("PlaybackMode - time and volume changes preserve adjacent button positions",
            "[tui][regression][playback-mode]")
  {
    auto state = rt::PlaybackTransportSnapshot{.duration = std::chrono::minutes{20}};
    state.volume.level = 0.99F;
    auto nextState = state;
    auto const elapsed = std::chrono::seconds{599};
    auto nextElapsed = elapsed;

    SECTION("elapsed gains a digit")
    {
      nextElapsed = std::chrono::seconds{600};
    }

    SECTION("volume reaches one hundred percent")
    {
      nextState.volume.level = 1.0F;
    }

    SECTION("muted label replaces volume")
    {
      nextState.volume.muted = true;
    }

    for (std::string_view const locale : {"en", "zh-Hans", "zh-Hant", "ja", "de", "es", "fr"})
    {
      auto const catalog = ao::test::messageCatalog(locale);

      for (auto const width : {48, 140})
      {
        CAPTURE(locale, width);
        auto output = uimodel::OutputDeviceViewState{.outputBackendSummary = "PW", .hasActiveOutputDevice = true};
        auto modeBox = kEmptyMouseBox;
        auto outputBox = kEmptyMouseBox;
        auto view = PlaybackBarViewState{.playbackState = &state,
                                         .displayElapsed = elapsed,
                                         .outputView = &output,
                                         .outputDeviceBox = &outputBox,
                                         .playbackModeBox = &modeBox,
                                         .terminalColumns = width};
        auto const before = renderElement(playbackBar(catalog, view), width, 1);
        auto const beforeMode = modeBox;
        auto const beforeOutput = outputBox;
        REQUIRE_FALSE(beforeMode.IsEmpty());
        REQUIRE_FALSE(beforeOutput.IsEmpty());
        view.playbackState = &nextState;
        view.displayElapsed = nextElapsed;
        auto const after = renderElement(playbackBar(catalog, view), width, 1);
        INFO(before.text);
        INFO(after.text);
        CHECK(modeBox.x_min == beforeMode.x_min);
        CHECK(modeBox.x_max == beforeMode.x_max);
        CHECK(outputBox.x_min == beforeOutput.x_min);
        CHECK(outputBox.x_max == beforeOutput.x_max);
        CHECK_FALSE(outputBox.Contain(beforeMode.x_max, beforeMode.y_min));
      }
    }
  }

  TEST_CASE("PlaybackPanel - narrow volume slots shorten mute wording without losing the title or numeric value",
            "[tui][regression][playback][playback-mode]")
  {
    auto const catalog = ao::test::messageCatalog("de");

    for (auto const& [width, mutedLabel] : std::array{std::pair{48, "Stummgescha…"}, std::pair{140, "Stummgeschaltet"}})
    {
      CAPTURE(width);
      auto state = rt::PlaybackTransportSnapshot{.nowPlaying = {.title = "Sample song"}};
      auto volumeBox = kEmptyMouseBox;
      auto const view =
        PlaybackBarViewState{.playbackState = &state, .volumeBox = &volumeBox, .terminalColumns = width};
      auto const normal = renderElement(playbackBar(catalog, view), width, 1);
      auto const normalBox = volumeBox;
      CHECK(normal.text.contains("Sample"));
      CHECK(normal.text.contains("100%"));
      state.volume.muted = true;
      auto const muted = renderElement(playbackBar(catalog, view), width, 1);
      INFO(muted.text);
      CHECK(muted.text.contains("Sample"));
      CHECK(muted.text.contains(mutedLabel));
      CHECK(volumeBox.x_min == normalBox.x_min);
      CHECK(volumeBox.x_max == normalBox.x_max);
    }
  }

  TEST_CASE("PlaybackMode - hover identifies the current and next preset for every mode combination",
            "[tui][unit][playback-mode]")
  {
    for (auto const& [choice, hint] : std::array{
           std::pair{PlaybackModeChoice{}, "In order · Click for Repeat all"},
           std::pair{PlaybackModeChoice{.repeat = rt::RepeatMode::All}, "Repeat all · Click for Shuffle"},
           std::pair{PlaybackModeChoice{.shuffle = rt::ShuffleMode::On}, "Shuffle · Click for Shuffle and repeat"},
           std::pair{PlaybackModeChoice{.shuffle = rt::ShuffleMode::On, .repeat = rt::RepeatMode::All},
                     "Shuffle and repeat · Click for Repeat one"},
           std::pair{PlaybackModeChoice{.repeat = rt::RepeatMode::One}, "Repeat one · Click for In order"},
           std::pair{PlaybackModeChoice{.shuffle = rt::ShuffleMode::On, .repeat = rt::RepeatMode::One},
                     "Shuffle and repeat one · Click for In order"}})
    {
      CAPTURE(hint);
      auto mode = rt::PlaybackSuccessionSnapshot{.shuffle = choice.shuffle, .repeat = choice.repeat};
      auto const rendered = renderElement(statusBar(ao::test::englishMessageCatalog(),
                                                    {.terminalColumns = 160, .hoveredPlaybackMode = &mode},
                                                    defaultKeymapPlan()),
                                          160,
                                          1);
      CHECK(rendered.text.ends_with(hint));
    }
  }

  TEST_CASE("PlaybackMode - repeat-one hover preserves the shuffle preference in every language",
            "[tui][unit][playback-mode]")
  {
    auto mode = rt::PlaybackSuccessionSnapshot{.shuffle = rt::ShuffleMode::On, .repeat = rt::RepeatMode::One};

    for (auto const& [locale, label] : std::array{std::pair{"en", "Shuffle and repeat one"},
                                                  std::pair{"zh-Hans", "随机播放与单曲循环"},
                                                  std::pair{"zh-Hant", "隨機播放與單曲循環"},
                                                  std::pair{"ja", "シャッフル・1曲リピート"},
                                                  std::pair{"de", "Zufallswiedergabe mit Titelwiederholung"},
                                                  std::pair{"es", "Aleatorio con repetición de una pista"},
                                                  std::pair{"fr", "Lecture aléatoire avec répétition d’un titre"}})
    {
      auto const catalog = ao::test::messageCatalog(locale);

      for (auto const width : {48, 160})
      {
        CAPTURE(locale, width);
        auto const rendered = renderElement(
          statusBar(catalog, {.terminalColumns = width, .hoveredPlaybackMode = &mode}, defaultKeymapPlan()), width, 1);
        INFO(rendered.text);
        CHECK(rendered.text.contains(label));
      }
    }
  }

  TEST_CASE("PlaybackMode - localized hover hints prioritize the current mode at narrow widths",
            "[tui][unit][playback-mode]")
  {
    auto mode = rt::PlaybackSuccessionSnapshot{.shuffle = rt::ShuffleMode::On, .repeat = rt::RepeatMode::All};
    auto activity =
      uimodel::ActivityStatusViewState{.compact = {.kind = uimodel::ActivityStatusKind::Info, .text = "Saved"}};
    auto activityBox = kEmptyMouseBox;

    struct Expected final
    {
      std::string_view locale;
      std::string_view current;
      std::string_view next;
      std::string_view hint;
    };

    for (auto const& expected : std::array{
           Expected{"en", "Shuffle and repeat", "Repeat one", "Shuffle and repeat · Click for Repeat one"},
           Expected{"zh-Hans", "随机循环", "单曲循环", "随机循环 · 点击切换为「单曲循环」"},
           Expected{"zh-Hant", "隨機循環", "單曲循環", "隨機循環 · 點擊切換為「單曲循環」"},
           Expected{
             "ja", "シャッフルリピート", "1曲リピート", "シャッフルリピート · クリックで「1曲リピート」に切り替え"},
           Expected{"de",
                    "Zufallswiedergabe mit Wiederholung",
                    "Einen Titel wiederholen",
                    "Zufallswiedergabe mit Wiederholung · Klicken für „Einen Titel wiederholen“"},
           Expected{"es",
                    "Aleatorio con repetición",
                    "Repetir una pista",
                    "Aleatorio con repetición · Clic para cambiar a «Repetir una pista»"},
           Expected{"fr",
                    "Lecture aléatoire en boucle",
                    "Répéter un titre",
                    "Lecture aléatoire en boucle · Cliquer pour passer à « Répéter un titre »"}})
    {
      auto const catalog = ao::test::messageCatalog(expected.locale);

      for (auto const width : {48, 160})
      {
        CAPTURE(expected.locale, width);
        auto const rendered = renderElement(statusBar(catalog,
                                                      {.activityStatus = &activity,
                                                       .terminalColumns = width,
                                                       .activityStatusBox = &activityBox,
                                                       .hoveredPlaybackMode = &mode},
                                                      defaultKeymapPlan()),
                                            width,
                                            1);
        INFO(rendered.text);
        auto const optCurrentBox = findTextCells(rendered.screen, expected.current);
        REQUIRE(optCurrentBox);
        CHECK(activityBox.x_max < optCurrentBox->x_min);
        CHECK_FALSE(rendered.text.contains("tui_playback_mode"));
        CHECK_FALSE(rendered.text.contains("{current}"));

        if (width == 160)
        {
          CHECK(rendered.text.ends_with(expected.hint));
        }
        else
        {
          CHECK_FALSE(rendered.text.contains(expected.next));
          CHECK(rendered.text.ends_with(expected.current));
        }
      }
    }
  }

  TEST_CASE("PlaybackMode - narrow activity slots retain content or disappear", "[tui][regression][playback-mode]")
  {
    auto const catalog = ao::test::messageCatalog("ja");
    auto mode = rt::PlaybackSuccessionSnapshot{.shuffle = rt::ShuffleMode::On, .repeat = rt::RepeatMode::All};
    auto activity =
      uimodel::ActivityStatusViewState{.compact = {.kind = uimodel::ActivityStatusKind::Info, .text = "Saved"}};
    auto box = kEmptyMouseBox;

    for (auto const columns : {26, 24, 25})
    {
      auto const rendered = renderElement(statusBar(catalog,
                                                    {.activityStatus = &activity,
                                                     .terminalColumns = columns,
                                                     .activityStatusBox = &box,
                                                     .hoveredPlaybackMode = &mode},
                                                    defaultKeymapPlan()),
                                          columns,
                                          1);
      INFO(rendered.text);
      CHECK(rendered.text.ends_with("シャッフルリピート"));

      if (columns == 26)
      {
        REQUIRE_FALSE(box.IsEmpty());
        CHECK(box.x_max - box.x_min + 1 == 5);
        CHECK(rendered.screen.PixelAt(box.x_min + 2, box.y_min).character == "i");
      }
      else
      {
        CHECK(box.IsEmpty());
        CHECK_FALSE(rendered.text.contains("│"));
      }
    }
  }

  TEST_CASE("PlaybackMode - hover retires hidden shortcuts and yields to foreground hints",
            "[tui][unit][playback-mode]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto shell = ShellInteractionModel{};
    auto mode = rt::PlaybackSuccessionSnapshot{};
    auto settingsBox = kEmptyMouseBox;
    auto actions = std::list<StatusActionHitRegion>{};
    auto state = StatusBarViewState{
      .terminalColumns = 120, .shell = &shell, .settingsButtonBox = &settingsBox, .actionHitRegions = &actions};
    auto render = [&] { return renderElement(statusBar(catalog, state, defaultKeymapPlan()), 120, 1).text; };
    CHECK(render().contains("Settings"));
    REQUIRE_FALSE(settingsBox.IsEmpty());
    REQUIRE_FALSE(actions.empty());
    state.hoveredPlaybackMode = &mode;
    CHECK(render().contains("In order · Click for Repeat all"));
    CHECK(settingsBox.IsEmpty());
    CHECK(actions.empty());
    shell.focusNavigation();
    CHECK(render().contains("In order · Click for Repeat all"));
    shell.focusDetail();
    CHECK(render().contains("In order · Click for Repeat all"));
    shell.beginInput(ShellInputMode::QuickFilter, "artist");
    CHECK_FALSE(render().contains("Click for"));
    shell.closeInput();
    shell.openOverlay(Overlay::GoTo);
    CHECK_FALSE(render().contains("Click for"));
    shell.openOverlay(Overlay::Help);
    CHECK_FALSE(render().contains("Click for"));
    shell.closeOverlay();
    shell.focusTracks();
    state.optResizingDivider = PanelDivider::Navigation;
    CHECK_FALSE(render().contains("Click for"));
    state.optResizingDivider.reset();
    CHECK(render().contains("Click for"));
    state.hoveredPlaybackMode = nullptr;
    CHECK(render().contains("Settings"));
    CHECK_FALSE(settingsBox.IsEmpty());
    CHECK_FALSE(actions.empty());
  }

  TEST_CASE("PlaybackMode - hover preserves visual selection controls", "[tui][regression][playback-mode]")
  {
    auto mode = rt::PlaybackSuccessionSnapshot{};
    auto cancelBox = kEmptyMouseBox;
    auto const rendered = renderElement(statusBar(ao::test::englishMessageCatalog(),
                                                  {.terminalColumns = 120,
                                                   .visualSelectionActive = true,
                                                   .cancelSelectionBox = &cancelBox,
                                                   .hoveredPlaybackMode = &mode},
                                                  defaultKeymapPlan()),
                                        120,
                                        1);
    INFO(rendered.text);
    CHECK(rendered.text.contains("VISUAL"));
    CHECK(rendered.text.contains("keep"));
    CHECK(rendered.text.contains("Esc cancel"));
    CHECK_FALSE(rendered.text.contains("Click for"));
    CHECK_FALSE(cancelBox.IsEmpty());
  }

  TEST_CASE("PlaybackMode - invalid closed enum values retire the button and retain ordinary status hints",
            "[tui][regression][playback-mode]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto const invalidShuffle = rt::PlaybackSuccessionSnapshot{.shuffle = static_cast<rt::ShuffleMode>(2)};
    auto const invalidRepeat = rt::PlaybackSuccessionSnapshot{.repeat = static_cast<rt::RepeatMode>(3)};

    for (auto const* mode : {&invalidShuffle, &invalidRepeat})
    {
      auto box = kEmptyMouseBox;
      auto view = PlaybackBarViewState{.playbackModeBox = &box, .terminalColumns = 120};
      renderElement(playbackBar(catalog, view), 120, 1);
      REQUIRE_FALSE(box.IsEmpty());
      auto const previousBox = box;
      view.succession = mode;
      view.playbackModeHovered = true;
      auto const playback = renderElement(playbackBar(catalog, view), 120, 1);
      CHECK(box.IsEmpty());

      for (auto column = previousBox.x_min; column <= previousBox.x_max; ++column)
      {
        CHECK(playback.screen.PixelAt(column, previousBox.y_min).background_color == ftxui::Color::Default);
      }

      auto const rendered = renderElement(
        statusBar(catalog, {.terminalColumns = 120, .hoveredPlaybackMode = mode}, defaultKeymapPlan()), 120, 1);
      CHECK(rendered.text.contains("Settings"));
      CHECK_FALSE(rendered.text.contains("Click for"));
    }
  }
} // namespace ao::tui::test

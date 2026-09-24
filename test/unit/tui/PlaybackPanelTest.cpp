// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/PlaybackPanel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include <ao/audio/Quality.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ao::tui::test
{
  namespace
  {
    ftxui::Element englishPlaybackBar(PlaybackBarViewState const& view)
    {
      return playbackBar(ao::test::englishMessageCatalog(), view);
    }

    bool isBrailleGlyph(std::string const& character)
    {
      return character.size() == 3 && static_cast<unsigned char>(character[0]) == 0xE2 &&
             (static_cast<unsigned char>(character[1]) & 0xFC) == 0xA0;
    }

    bool containsBrailleGlyph(std::string const& text)
    {
      for (std::size_t index = 0; index + 1 < text.size(); ++index)
      {
        if (static_cast<unsigned char>(text[index]) == 0xE2 &&
            (static_cast<unsigned char>(text[index + 1]) & 0xFC) == 0xA0)
        {
          return true;
        }
      }

      return false;
    }

    std::int32_t cellIndexOf(ftxui::Screen const& screen, std::string const& needle, std::int32_t const row = 0)
    {
      auto const needleColumns = static_cast<std::int32_t>(needle.size());

      for (std::int32_t column = 0; column <= screen.dimx() - needleColumns; ++column)
      {
        bool matches = true;

        for (std::int32_t offset = 0; offset < needleColumns; ++offset)
        {
          auto const needleCell = std::string(1, needle[static_cast<std::size_t>(offset)]);

          if (screen.PixelAt(column + offset, row).character != needleCell)
          {
            matches = false;
            break;
          }
        }

        if (matches)
        {
          return column;
        }
      }

      return -1;
    }
  } // namespace

  TEST_CASE("PlaybackPanel - playback bar renders idle fallback state", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackTransportSnapshot{};

    auto const text = renderText(englishPlaybackBar(PlaybackBarViewState{.playbackState = &state}), 96);

    CHECK_FALSE(text.contains("Aobus"));
    CHECK_FALSE(text.contains("Library"));
    CHECK_FALSE(text.contains("view:"));
    CHECK(containsBrailleGlyph(text));
    CHECK(text.contains("No active track"));
    CHECK(text.contains('-'));
    CHECK(text.contains("0:00"));
    CHECK(text.contains("--:--"));
    CHECK(text.contains("Vol 100%"));
  }

  TEST_CASE("PlaybackPanel - playback bar uses localized copy without changing track data",
            "[tui][unit][playback][localization]")
  {
    auto const german = ao::test::messageCatalog("de-DE");
    auto state = rt::PlaybackTransportSnapshot{
      .nowPlaying = rt::NowPlayingInfo{.title = "誰か、海を。"}, .volume = rt::VolumeState{.level = 0.42F}};
    auto text = renderText(playbackBar(german, {.playbackState = &state}), 96);

    CHECK(text.contains("誰か、海を。"));
    CHECK(text.contains("Lautst. 42%"));

    state.nowPlaying.title.clear();
    text = renderText(playbackBar(german, {.playbackState = &state}), 96);
    CHECK(text.contains("Kein aktiver Titel"));
  }

  TEST_CASE("PlaybackPanel - muted volume remains visible as a mouse target", "[tui][unit][playback][mouse]")
  {
    auto state = rt::PlaybackTransportSnapshot{.volume = rt::VolumeState{.level = 0.42F, .muted = true}};
    auto box = ftxui::Box{};
    auto screen = ftxui::Screen{100, 8};
    ftxui::Render(screen, englishPlaybackBar(PlaybackBarViewState{.playbackState = &state, .volumeBox = &box}));
    CHECK(screen.ToString().contains("Muted"));
    CHECK_FALSE(screen.ToString().contains("42%"));
    CHECK_FALSE(box.IsEmpty());
    auto const optRenderedMutedBox = findTextCells(screen, "Muted");
    REQUIRE(optRenderedMutedBox);
    CHECK(box.x_min <= optRenderedMutedBox->x_min);
    CHECK(box.x_max == optRenderedMutedBox->x_max);
    CHECK(box.y_min == optRenderedMutedBox->y_min);
    CHECK(box.y_max >= optRenderedMutedBox->y_max);
    CHECK(box.y_max < screen.dimy());
  }

  TEST_CASE("PlaybackPanel - playback bar renders current track timing and volume", "[tui][unit][playback]")
  {
    auto state =
      rt::PlaybackTransportSnapshot{.transport = audio::Transport::Playing,
                                    .duration = std::chrono::seconds{125},
                                    .nowPlaying = rt::NowPlayingInfo{.title = "Signal Path", .artist = "Artist"},
                                    .volume = rt::VolumeState{.level = 0.42F},
                                    .quality = rt::QualityState{.overall = audio::Quality::LosslessFloat}};

    auto const text = renderText(
      englishPlaybackBar(PlaybackBarViewState{.playbackState = &state, .displayElapsed = std::chrono::seconds{65}}),
      96);

    CHECK_FALSE(text.contains("view:"));
    CHECK(text.contains("Signal Path"));
    CHECK(text.contains("Artist"));
    CHECK_FALSE(text.contains("Aobus"));
    CHECK(containsBrailleGlyph(text));
    CHECK(text.contains("1:05"));
    CHECK(text.contains("2:05"));
    CHECK(text.contains("Vol 42%"));
  }

  TEST_CASE("PlaybackPanel - paused soul retains its sampled frame while live quality changes",
            "[tui][unit][playback][soul]")
  {
    auto state = rt::PlaybackTransportSnapshot{
      .transport = audio::Transport::Paused,
      .ready = true,
      .quality = rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                  .pipelineQuality = audio::Quality::BitwisePerfect,
                                  .overall = audio::Quality::BitwisePerfect},
    };
    auto const renderAt = [&state](std::chrono::milliseconds const animationElapsed)
    {
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(96), ftxui::Dimension::Fixed(1));
      ftxui::Render(screen,
                    englishPlaybackBar(PlaybackBarViewState{
                      .playbackState = &state,
                      .animationElapsed = animationElapsed,
                      .soulMotion = uimodel::aobusSoulMotionAt(std::chrono::milliseconds{2080}),
                    }));
      return screen;
    };

    auto const early = renderAt(std::chrono::milliseconds{0});
    auto const late = renderAt(std::chrono::milliseconds{5120});
    CHECK(early.PixelAt(0, 0).character == "⠚");
    CHECK(early.PixelAt(1, 0).character == "⠉");
    CHECK(early.PixelAt(2, 0).character == "⠓");
    CHECK(late.PixelAt(0, 0).character == early.PixelAt(0, 0).character);
    CHECK(late.PixelAt(1, 0).character == early.PixelAt(1, 0).character);
    CHECK(late.PixelAt(2, 0).character == early.PixelAt(2, 0).character);
    CHECK(late.PixelAt(1, 0).foreground_color == early.PixelAt(1, 0).foreground_color);

    state.quality.pipelineQuality = audio::Quality::LinearIntervention;
    state.quality.overall = audio::Quality::LinearIntervention;
    auto const changed = renderAt(std::chrono::milliseconds{5120});

    CHECK(changed.PixelAt(0, 0).character == early.PixelAt(0, 0).character);
    CHECK(changed.PixelAt(1, 0).character == early.PixelAt(1, 0).character);
    CHECK(changed.PixelAt(2, 0).character == early.PixelAt(2, 0).character);
    CHECK_FALSE(changed.PixelAt(1, 0).foreground_color == early.PixelAt(1, 0).foreground_color);
  }

  TEST_CASE("PlaybackPanel - playback bar anchors the soul button at the far left", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackTransportSnapshot{};
    auto soulButtonBox = ftxui::Box{};

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(96), ftxui::Dimension::Fixed(1));
    ftxui::Render(
      screen, englishPlaybackBar(PlaybackBarViewState{.playbackState = &state, .soulButtonBox = &soulButtonBox}));

    CHECK(isBrailleGlyph(screen.PixelAt(1, 0).character));
    CHECK(soulButtonBox.x_min == 0);
    CHECK(soulButtonBox.x_max == 2);
    CHECK(soulButtonBox.y_min == 0);
  }

  TEST_CASE("PlaybackPanel - playback bar previews seek position without percent text", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackTransportSnapshot{.transport = audio::Transport::Playing,
                                               .duration = std::chrono::seconds{100},
                                               .nowPlaying = rt::NowPlayingInfo{.title = "Signal Path"}};

    auto const text = renderText(
      englishPlaybackBar(PlaybackBarViewState{.playbackState = &state, .displayElapsed = std::chrono::seconds{50}}),
      96);

    CHECK_FALSE(text.contains("50%"));
    CHECK(text.contains("●"));
    CHECK(text.contains("1:40"));
  }

  TEST_CASE("PlaybackPanel - playback bar reflects only the seek rail", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackTransportSnapshot{.transport = audio::Transport::Playing,
                                               .duration = std::chrono::seconds{100},
                                               .nowPlaying = rt::NowPlayingInfo{.title = "Signal Path"}};
    auto seekRailBox = ftxui::Box{};

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(96), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen,
                  englishPlaybackBar(PlaybackBarViewState{
                    .playbackState = &state, .displayElapsed = std::chrono::seconds{50}, .seekRailBox = &seekRailBox}));

    CHECK(screen.ToString().contains("0:50"));
    CHECK(seekRailBox.y_min == 0);
    CHECK(seekRailBox.y_max == 0);
    CHECK(seekRailBox.x_min < seekRailBox.x_max);
    CHECK(seekRailBox.x_min > 0);
  }

  TEST_CASE("PlaybackPanel - playback bar places output selector before elapsed time", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackTransportSnapshot{
      .duration = std::chrono::seconds{100}, .nowPlaying = rt::NowPlayingInfo{.title = "Signal Path"}};
    auto output = uimodel::OutputDeviceViewState{
      .outputBackendSummary = "PW",
      .outputDeviceStatus = "PipeWire: Studio DAC",
      .hasActiveOutputDevice = true,
    };
    auto outputDeviceBox = ftxui::Box{};
    auto seekRailBox = ftxui::Box{};

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen,
                  englishPlaybackBar(PlaybackBarViewState{.playbackState = &state,
                                                          .outputView = &output,
                                                          .outputDeviceBox = &outputDeviceBox,
                                                          .seekRailBox = &seekRailBox,
                                                          .terminalColumns = 120}));

    auto const elapsedColumn = cellIndexOf(screen, "0:00");
    REQUIRE(elapsedColumn >= 0);
    CHECK(outputDeviceBox.x_max + 1 == elapsedColumn);
    CHECK(elapsedColumn < seekRailBox.x_min);
    CHECK(outputDeviceBox.y_min == seekRailBox.y_min);
  }

  TEST_CASE("PlaybackPanel - playback bar expands seek rail on wide terminals", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackTransportSnapshot{
      .duration = std::chrono::seconds{100}, .nowPlaying = rt::NowPlayingInfo{.title = "Signal Path"}};
    auto narrowRailBox = ftxui::Box{};
    auto wideRailBox = ftxui::Box{};

    auto narrowScreen = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(1));
    ftxui::Render(narrowScreen,
                  englishPlaybackBar(PlaybackBarViewState{
                    .playbackState = &state, .seekRailBox = &narrowRailBox, .terminalColumns = 72}));

    auto wideScreen = ftxui::Screen::Create(ftxui::Dimension::Fixed(160), ftxui::Dimension::Fixed(1));
    ftxui::Render(wideScreen,
                  englishPlaybackBar(PlaybackBarViewState{
                    .playbackState = &state, .seekRailBox = &wideRailBox, .terminalColumns = 150}));

    REQUIRE_FALSE(narrowRailBox.IsEmpty());
    CHECK(narrowRailBox.x_max - narrowRailBox.x_min + 1 == 24);
    CHECK(wideRailBox.x_max - wideRailBox.x_min + 1 == 48);
  }

  TEST_CASE("PlaybackPanel - playback dock stays on one row", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackTransportSnapshot{.transport = audio::Transport::Playing,
                                               .duration = std::chrono::seconds{100},
                                               .nowPlaying = rt::NowPlayingInfo{.title = "Signal Path"}};
    auto seekRailBox = ftxui::Box{};

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen,
                  englishPlaybackBar(PlaybackBarViewState{
                    .playbackState = &state, .displayElapsed = std::chrono::seconds{50}, .seekRailBox = &seekRailBox}));

    CHECK(playbackBarRows(20) == 1);
    CHECK(playbackBarRows(24) == 1);
    CHECK(screen.ToString().contains("Signal Path"));
    CHECK(screen.ToString().contains("0:50"));
    CHECK(screen.ToString().contains("1:40"));
    CHECK(seekRailBox.y_min == 0);
    CHECK(seekRailBox.y_max == 0);
  }

  TEST_CASE("PlaybackPanel - playback bar renders output backend badge", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackTransportSnapshot{};
    auto const output = uimodel::OutputDeviceViewState{
      .outputBackendSummary = "PW",
      .outputDeviceStatus = "PipeWire: Studio DAC",
      .hasActiveOutputDevice = true,
    };

    auto const text =
      renderText(englishPlaybackBar(PlaybackBarViewState{.playbackState = &state, .outputView = &output}), 96);

    CHECK(text.contains("PW"));
    CHECK(text.contains("No active track"));
  }

  TEST_CASE("PlaybackPanel - hovered output selector uses the interactive surface", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackTransportSnapshot{};
    auto const output = uimodel::OutputDeviceViewState{
      .outputBackendSummary = "PW",
      .outputDeviceStatus = "PipeWire: Studio DAC",
      .hasActiveOutputDevice = true,
    };
    auto outputDeviceBox = ftxui::Box{};

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(96), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen,
                  englishPlaybackBar(PlaybackBarViewState{.playbackState = &state,
                                                          .outputView = &output,
                                                          .outputDeviceBox = &outputDeviceBox,
                                                          .outputDeviceHovered = true}));

    auto const pixel = screen.PixelAt(outputDeviceBox.x_min, outputDeviceBox.y_min);
    checkInteractiveSurface(pixel);
  }
} // namespace ao::tui::test

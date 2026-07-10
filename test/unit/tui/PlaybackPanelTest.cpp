// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/PlaybackPanel.h"

#include "test/unit/tui/TuiRenderTestSupport.h"
#include "tui/OutputDevicePanel.h"
#include "tui/QualityPanel.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/Transport.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    std::string renderPlaybackText(ftxui::Element elementPtr)
    {
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(96), ftxui::Dimension::Fit(elementPtr));
      ftxui::Render(screen, elementPtr);
      return screen.ToString();
    }

    std::string renderPlaybackText(ftxui::Element elementPtr, std::int32_t const width, std::int32_t const height)
    {
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
      ftxui::Render(screen, elementPtr);
      return screen.ToString();
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

    audio::Format cdFormat()
    {
      return audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .validBits = 16};
    }
  } // namespace

  TEST_CASE("PlaybackPanel - playback bar renders idle fallback state", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackState{};

    auto const text = renderPlaybackText(playbackBar(PlaybackBarViewState{.playbackState = &state}));

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

  TEST_CASE("PlaybackPanel - playback bar renders current track timing and volume", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackState{.transport = audio::Transport::Playing,
                                   .duration = std::chrono::seconds{125},
                                   .nowPlaying = rt::NowPlayingInfo{.title = "Signal Path", .artist = "Artist"},
                                   .volume = rt::VolumeState{.level = 0.42F},
                                   .quality = rt::QualityState{.overall = audio::Quality::LosslessFloat}};

    auto const text = renderPlaybackText(
      playbackBar(PlaybackBarViewState{.playbackState = &state, .displayElapsed = std::chrono::seconds{65}}));

    CHECK_FALSE(text.contains("view:"));
    CHECK(text.contains("Signal Path"));
    CHECK(text.contains("Artist"));
    CHECK_FALSE(text.contains("Aobus"));
    CHECK(containsBrailleGlyph(text));
    CHECK(text.contains("1:05"));
    CHECK(text.contains("2:05"));
    CHECK(text.contains("Vol 42%"));
  }

  TEST_CASE("PlaybackPanel - playback bar anchors the soul button at the far left", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackState{};
    auto soulButtonBox = ftxui::Box{};

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(96), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen, playbackBar(PlaybackBarViewState{.playbackState = &state, .soulButtonBox = &soulButtonBox}));

    CHECK(isBrailleGlyph(screen.PixelAt(1, 0).character));
    CHECK(soulButtonBox.x_min == 0);
    CHECK(soulButtonBox.x_max == 2);
    CHECK(soulButtonBox.y_min == 0);
  }

  TEST_CASE("PlaybackPanel - playback bar previews seek position without percent text", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackState{.transport = audio::Transport::Playing,
                                   .duration = std::chrono::seconds{100},
                                   .nowPlaying = rt::NowPlayingInfo{.title = "Signal Path"}};

    auto const text = renderPlaybackText(
      playbackBar(PlaybackBarViewState{.playbackState = &state, .displayElapsed = std::chrono::seconds{50}}));

    CHECK_FALSE(text.contains("50%"));
    CHECK(text.contains("●"));
    CHECK(text.contains("1:40"));
  }

  TEST_CASE("PlaybackPanel - playback bar reflects only the seek rail", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackState{.transport = audio::Transport::Playing,
                                   .duration = std::chrono::seconds{100},
                                   .nowPlaying = rt::NowPlayingInfo{.title = "Signal Path"}};
    auto seekRailBox = ftxui::Box{};

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(96), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen,
                  playbackBar(PlaybackBarViewState{
                    .playbackState = &state, .displayElapsed = std::chrono::seconds{50}, .seekRailBox = &seekRailBox}));

    CHECK(screen.ToString().contains("0:50"));
    CHECK(seekRailBox.y_min == 0);
    CHECK(seekRailBox.y_max == 0);
    CHECK(seekRailBox.x_min < seekRailBox.x_max);
    CHECK(seekRailBox.x_min > 0);
  }

  TEST_CASE("PlaybackPanel - playback bar places output selector before elapsed time", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackState{
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
                  playbackBar(PlaybackBarViewState{.playbackState = &state,
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
    auto const state = rt::PlaybackState{
      .duration = std::chrono::seconds{100}, .nowPlaying = rt::NowPlayingInfo{.title = "Signal Path"}};
    auto narrowRailBox = ftxui::Box{};
    auto wideRailBox = ftxui::Box{};

    auto narrowScreen = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(1));
    ftxui::Render(
      narrowScreen,
      playbackBar(PlaybackBarViewState{.playbackState = &state, .seekRailBox = &narrowRailBox, .terminalColumns = 72}));

    auto wideScreen = ftxui::Screen::Create(ftxui::Dimension::Fixed(160), ftxui::Dimension::Fixed(1));
    ftxui::Render(
      wideScreen,
      playbackBar(PlaybackBarViewState{.playbackState = &state, .seekRailBox = &wideRailBox, .terminalColumns = 150}));

    CHECK(narrowRailBox.x_max - narrowRailBox.x_min + 1 == 24);
    CHECK(wideRailBox.x_max - wideRailBox.x_min + 1 == 48);
  }

  TEST_CASE("PlaybackPanel - playback dock stays on one row", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackState{.transport = audio::Transport::Playing,
                                   .duration = std::chrono::seconds{100},
                                   .nowPlaying = rt::NowPlayingInfo{.title = "Signal Path"}};
    auto seekRailBox = ftxui::Box{};

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen,
                  playbackBar(PlaybackBarViewState{
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
    auto const state = rt::PlaybackState{};
    auto const output = uimodel::OutputDeviceViewState{
      .outputBackendSummary = "PW",
      .outputDeviceStatus = "PipeWire: Studio DAC",
      .hasActiveOutputDevice = true,
    };

    auto const text =
      renderPlaybackText(playbackBar(PlaybackBarViewState{.playbackState = &state, .outputView = &output}));

    CHECK(text.contains("PW"));
    CHECK(text.contains("No active track"));
  }

  TEST_CASE("PlaybackPanel - hovered output selector uses the interactive surface", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackState{};
    auto const output = uimodel::OutputDeviceViewState{
      .outputBackendSummary = "PW",
      .outputDeviceStatus = "PipeWire: Studio DAC",
      .hasActiveOutputDevice = true,
    };
    auto outputDeviceBox = ftxui::Box{};

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(96), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen,
                  playbackBar(PlaybackBarViewState{.playbackState = &state,
                                                   .outputView = &output,
                                                   .outputDeviceBox = &outputDeviceBox,
                                                   .outputDeviceHovered = true}));

    auto const pixel = screen.PixelAt(outputDeviceBox.x_min, outputDeviceBox.y_min);
    checkInteractiveSurface(pixel);
  }

  TEST_CASE("PlaybackPanel - quality panel renders empty pipeline state", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackState{.quality = rt::QualityState{.overall = audio::Quality::Unknown}};

    auto const text = renderPlaybackText(qualityPanel(state));

    CHECK_FALSE(text.contains("Quality"));
    CHECK_FALSE(text.contains("Audio Pipeline"));
    CHECK(text.contains("No audio pipeline yet"));
    CHECK(text.contains("a toggle"));
    CHECK(text.contains("Esc close"));
  }

  TEST_CASE("PlaybackPanel - quality panel renders selected device pipeline and findings", "[tui][unit][playback]")
  {
    auto state =
      rt::PlaybackState{
        .output =
          rt::OutputState{
            .selectedDevice = rt::OutputDeviceSelection{.backendId = audio::BackendId{"mock_backend"},
                                                        .deviceId = audio::DeviceId{"dac"}},
            .availableBackends =
              std::vector{
                rt::OutputBackendSnapshot{
                  .id = audio::BackendId{"mock_backend"},
                  .name = "Mock Backend",
                  .devices =
                    std::vector{
                      rt::OutputDeviceSnapshot{.id = audio::DeviceId{"dac"}, .displayName = "Studio DAC"},
                    },
                },
              },
          },
        .quality =
          rt::QualityState{
            .sourceQuality = audio::Quality::BitwisePerfect,
            .pipelineQuality = audio::Quality::LinearIntervention,
            .overall = audio::Quality::LinearIntervention,
            .assessments =
              std::vector{
                audio::NodeQualityAssessment{
                  .nodeId = "ao-source",
                  .nodeName = "FLAC",
                  .nodeType = audio::flow::NodeType::Source,
                  .optFormat = cdFormat(),
                  .worstQuality = audio::Quality::BitwisePerfect,
                  .findings =
                    std::vector{
                      audio::QualityFinding{
                        .kind = audio::QualityFindingKind::BitPerfect, .quality = audio::Quality::BitwisePerfect},
                    },
                },
                audio::NodeQualityAssessment{
                  .nodeId = "ao-sink",
                  .nodeName = "DAC",
                  .nodeType = audio::flow::NodeType::Sink,
                  .optFormat = cdFormat(),
                  .worstQuality = audio::Quality::LinearIntervention,
                  .findings =
                    std::vector{
                      audio::QualityFinding{
                        .kind = audio::QualityFindingKind::BitPerfect, .quality = audio::Quality::BitwisePerfect},
                      audio::QualityFinding{.kind = audio::QualityFindingKind::SoftwareVolumeModification,
                                            .quality = audio::Quality::LinearIntervention},
                    },
                },
              },
          },
      };

    auto const text = renderPlaybackText(qualityPanel(state));

    CHECK(text.contains("Studio DAC"));
    CHECK_FALSE(text.contains("Quality"));
    CHECK(text.contains("[Source] FLAC"));
    CHECK(text.contains("44.1 kHz"));
    CHECK(text.contains("[Device] DAC"));
    CHECK(text.contains("Software volume attenuation"));
    CHECK(text.contains("Pipeline intervention"));
  }

  TEST_CASE("PlaybackPanel - quality panel width follows content and terminal bounds", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackState{.quality = rt::QualityState{.overall = audio::Quality::Unknown}};
    auto const narrowColumns = qualityPanelColumns(state, 120);

    state.quality.assessments = std::vector{
      audio::NodeQualityAssessment{
        .nodeName = "Extremely Long Decoder Stage Name",
        .nodeType = audio::flow::NodeType::Source,
        .optFormat = cdFormat(),
      },
    };

    CHECK(qualityPanelColumns(state, 120) > narrowColumns);
    CHECK(qualityPanelColumns(state, 32) == 32);
  }

  TEST_CASE("PlaybackPanel - output device panel renders grouped selectable rows", "[tui][unit][playback]")
  {
    auto rowHitRegions = std::vector<OutputDeviceRowHitRegion>{};
    auto const view = uimodel::OutputDeviceViewState{
      .rows =
        std::vector{
          uimodel::OutputDeviceRow{
            .kind = uimodel::OutputDeviceRow::Kind::BackendHeader,
            .backendId = audio::BackendId{"pipewire"},
            .title = "PipeWire",
          },
          uimodel::OutputDeviceRow{
            .kind = uimodel::OutputDeviceRow::Kind::DeviceProfile,
            .backendId = audio::BackendId{"pipewire"},
            .deviceId = audio::DeviceId{"studio"},
            .profileId = audio::kProfileShared,
            .title = "Studio DAC",
            .description = "USB interface",
            .isActive = true,
          },
          uimodel::OutputDeviceRow{
            .kind = uimodel::OutputDeviceRow::Kind::DeviceProfile,
            .backendId = audio::BackendId{"pipewire"},
            .deviceId = audio::DeviceId{"studio"},
            .profileId = audio::kProfileExclusive,
            .title = "Studio DAC",
            .isExclusive = true,
          },
        },
      .outputBackendSummary = "PW",
      .outputDeviceStatus = "PipeWire: Studio DAC",
      .hasActiveOutputDevice = true,
    };

    auto const text = renderPlaybackText(outputDevicePanel(view, 2, &rowHitRegions));

    CHECK(text.contains("Output Devices"));
    CHECK(text.contains("PipeWire"));
    CHECK(text.contains("Studio DAC"));
    CHECK(text.contains("USB interface"));
    CHECK(text.contains("PipeWire: Studio DAC"));
    CHECK(text.contains("Enter select"));
    REQUIRE(rowHitRegions.size() == 2);
    CHECK(rowHitRegions[0].rowIndex == 1);
    CHECK(rowHitRegions[0].backendId == audio::BackendId{"pipewire"});
    CHECK(rowHitRegions[0].deviceId == audio::DeviceId{"studio"});
    CHECK(rowHitRegions[0].profileId == audio::kProfileShared);
    CHECK(rowHitRegions[1].rowIndex == 2);
    CHECK(rowHitRegions[1].profileId == audio::kProfileExclusive);
  }

  TEST_CASE("PlaybackPanel - output device panel width follows content and terminal bounds", "[tui][unit][playback]")
  {
    auto view = uimodel::OutputDeviceViewState{
      .rows =
        std::vector{
          uimodel::OutputDeviceRow{
            .kind = uimodel::OutputDeviceRow::Kind::DeviceProfile,
            .backendId = audio::BackendId{"pipewire"},
            .deviceId = audio::DeviceId{"studio"},
            .profileId = audio::kProfileShared,
            .title = "Studio DAC",
          },
        },
      .outputBackendSummary = "PW",
      .outputDeviceStatus = "PipeWire: Studio DAC",
    };
    auto const narrowColumns = outputDevicePanelColumns(view, 120);

    view.rows[0].description = "USB interface with a very long ALSA/PipeWire profile identifier";

    CHECK(outputDevicePanelColumns(view, 120) > narrowColumns);
    CHECK(outputDevicePanelColumns(view, 40) == 40);
  }

  TEST_CASE("PlaybackPanel - output device panel frames long device lists", "[tui][unit][playback]")
  {
    auto rows = std::vector{
      uimodel::OutputDeviceRow{
        .kind = uimodel::OutputDeviceRow::Kind::BackendHeader,
        .backendId = audio::BackendId{"pipewire"},
        .title = "PipeWire",
      },
    };

    for (std::int32_t index = 0; index < 20; ++index)
    {
      rows.push_back(uimodel::OutputDeviceRow{
        .kind = uimodel::OutputDeviceRow::Kind::DeviceProfile,
        .backendId = audio::BackendId{"pipewire"},
        .deviceId = audio::DeviceId{std::format("device-{}", index)},
        .profileId = audio::kProfileShared,
        .title = std::format("Device {}", index),
        .description = "alsa_output.usb-Sonata_Sonata_BHD_Pro_Sonata_BHD_Pro_very_long_identifier",
        .isActive = index == 0,
      });
    }

    auto const view = uimodel::OutputDeviceViewState{
      .rows = std::move(rows),
      .outputBackendSummary = "PW",
      .outputDeviceStatus = "PipeWire: Device 0",
      .hasActiveOutputDevice = true,
    };

    auto const text = renderPlaybackText(outputDevicePanel(view, 1), 48, 24);

    CHECK_FALSE(text.contains("very_long_identifier"));
    CHECK(text.contains("o toggle"));
  }
} // namespace ao::tui::test

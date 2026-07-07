// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/PlaybackPanel.h"

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
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
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    std::string renderText(ftxui::Element elementPtr)
    {
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(96), ftxui::Dimension::Fit(elementPtr));
      ftxui::Render(screen, elementPtr);
      return screen.ToString();
    }

    std::string renderText(ftxui::Element elementPtr, std::int32_t const width, std::int32_t const height)
    {
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
      ftxui::Render(screen, elementPtr);
      return screen.ToString();
    }

    audio::Format cdFormat()
    {
      return audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .validBits = 16};
    }
  } // namespace

  TEST_CASE("PlaybackPanel - playback bar renders idle fallback state", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackState{};

    auto const text = renderText(
      playbackBar(PlaybackBarViewState{.playbackState = &state, .listTitle = "Library", .presentationId = "songs"}));

    CHECK(text.find("Aobus") != std::string::npos);
    CHECK(text.find("Library") != std::string::npos);
    CHECK(text.find("view:songs") != std::string::npos);
    CHECK(text.find("No active track") != std::string::npos);
    CHECK(text.find("Idle") != std::string::npos);
    CHECK(text.find("0:00 / --:--") != std::string::npos);
    CHECK(text.find("100%") != std::string::npos);
  }

  TEST_CASE("PlaybackPanel - playback bar renders current track timing and volume", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackState{.transport = audio::Transport::Playing,
                                   .duration = std::chrono::seconds{125},
                                   .nowPlaying = rt::NowPlayingInfo{.title = "Signal Path", .artist = "Aobus"},
                                   .volume = rt::VolumeState{.level = 0.42F},
                                   .quality = rt::QualityState{.overall = audio::Quality::LosslessFloat}};

    auto const text = renderText(playbackBar(PlaybackBarViewState{.playbackState = &state,
                                                                  .listTitle = "Favorites",
                                                                  .presentationId = "albums",
                                                                  .displayElapsed = std::chrono::seconds{65}}));

    CHECK(text.find("Favorites") != std::string::npos);
    CHECK(text.find("view:albums") != std::string::npos);
    CHECK(text.find("Signal Path") != std::string::npos);
    CHECK(text.find("Aobus") != std::string::npos);
    CHECK(text.find("Playing") != std::string::npos);
    CHECK(text.find("1:05 / 2:05") != std::string::npos);
    CHECK(text.find("42%") != std::string::npos);
  }

  TEST_CASE("PlaybackPanel - playback bar renders output backend badge", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackState{};
    auto const output = uimodel::OutputDeviceViewState{
      .outputBackendSummary = "PW",
      .outputDeviceStatus = "PipeWire: Studio DAC",
      .hasActiveOutputDevice = true,
    };

    auto const text = renderText(playbackBar(PlaybackBarViewState{
      .playbackState = &state, .listTitle = "Library", .presentationId = "songs", .outputView = &output}));

    CHECK(text.find("PW") != std::string::npos);
    CHECK(text.find("No active track") != std::string::npos);
  }

  TEST_CASE("PlaybackPanel - playback bar renders default presentation fallback", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackState{};

    auto const text = renderText(playbackBar(PlaybackBarViewState{.playbackState = &state, .listTitle = "Library"}));

    CHECK(text.find("view:default") != std::string::npos);
  }

  TEST_CASE("PlaybackPanel - playback bar reflects presentation badge", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackState{};
    auto presentationBox = ftxui::Box{};

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(96), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen,
                  playbackBar(PlaybackBarViewState{.playbackState = &state,
                                                   .listTitle = "Library",
                                                   .presentationId = "albums",
                                                   .presentationBox = &presentationBox}));

    CHECK(screen.ToString().find("view:albums") != std::string::npos);
    CHECK(presentationBox.x_min > 0);
    CHECK(presentationBox.x_min < presentationBox.x_max);
    CHECK(presentationBox.y_min == 0);
  }

  TEST_CASE("PlaybackPanel - quality panel renders empty pipeline state", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackState{.quality = rt::QualityState{.overall = audio::Quality::Unknown}};

    auto const text = renderText(qualityPanel(state));

    CHECK(text.find("Audio Pipeline") != std::string::npos);
    CHECK(text.find("No audio pipeline yet") != std::string::npos);
    CHECK(text.find("a toggle") != std::string::npos);
    CHECK(text.find("Esc close") != std::string::npos);
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

    auto const text = renderText(qualityPanel(state));

    CHECK(text.find("Studio DAC") != std::string::npos);
    CHECK(text.find("[Source] FLAC") != std::string::npos);
    CHECK(text.find("44.1 kHz") != std::string::npos);
    CHECK(text.find("[Device] DAC") != std::string::npos);
    CHECK(text.find("Software volume attenuation") != std::string::npos);
    CHECK(text.find("Pipeline intervention") != std::string::npos);
  }

  TEST_CASE("PlaybackPanel - output device panel renders grouped selectable rows", "[tui][unit][playback]")
  {
    auto rowBoxes = std::vector<OutputDeviceRowBox>{};
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

    auto const text = renderText(outputDevicePanel(view, 2, &rowBoxes));

    CHECK(text.find("Output Devices") != std::string::npos);
    CHECK(text.find("PipeWire") != std::string::npos);
    CHECK(text.find("Studio DAC") != std::string::npos);
    CHECK(text.find("USB interface") != std::string::npos);
    CHECK(text.find("PipeWire: Studio DAC") != std::string::npos);
    CHECK(text.find("Enter select") != std::string::npos);
    REQUIRE(rowBoxes.size() == 2);
    CHECK(rowBoxes[0].rowIndex == 1);
    CHECK(rowBoxes[1].rowIndex == 2);
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

    auto const text = renderText(outputDevicePanel(view, 1), 48, 24);

    CHECK(text.find("very_long_identifier") == std::string::npos);
    CHECK(text.find("o toggle") != std::string::npos);
  }
} // namespace ao::tui::test

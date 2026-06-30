// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/PlaybackPanel.h"

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/Transport.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <chrono>
#include <string>
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

    audio::Format cdFormat()
    {
      return audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .validBits = 16};
    }
  } // namespace

  TEST_CASE("PlaybackPanel - playback bar renders idle fallback state", "[tui][unit][playback]")
  {
    auto const state = rt::PlaybackState{};

    auto const text = renderText(playbackBar(state, "Library", std::chrono::milliseconds{0}));

    CHECK(text.find("Aobus") != std::string::npos);
    CHECK(text.find("Library") != std::string::npos);
    CHECK(text.find("No active track") != std::string::npos);
    CHECK(text.find("Idle") != std::string::npos);
    CHECK(text.find("0:00 / --:--") != std::string::npos);
    CHECK(text.find("100%") != std::string::npos);
  }

  TEST_CASE("PlaybackPanel - playback bar renders current track timing and volume", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackState{.transport = audio::Transport::Playing,
                                   .trackTitle = "Signal Path",
                                   .trackArtist = "Aobus",
                                   .duration = std::chrono::seconds{125},
                                   .volume = 0.42F,
                                   .quality = audio::Quality::LosslessFloat};

    auto const text = renderText(playbackBar(state, "Favorites", std::chrono::seconds{65}));

    CHECK(text.find("Favorites") != std::string::npos);
    CHECK(text.find("Signal Path") != std::string::npos);
    CHECK(text.find("Aobus") != std::string::npos);
    CHECK(text.find("Playing") != std::string::npos);
    CHECK(text.find("1:05 / 2:05") != std::string::npos);
    CHECK(text.find("42%") != std::string::npos);
  }

  TEST_CASE("PlaybackPanel - quality panel renders empty pipeline state", "[tui][unit][playback]")
  {
    auto state = rt::PlaybackState{.quality = audio::Quality::Unknown};

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
        .selectedOutputDevice =
          rt::OutputDeviceSelection{.backendId = audio::BackendId{"mock_backend"}, .deviceId = audio::DeviceId{"dac"}},
        .availableOutputBackends =
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
        .flow =
          audio::flow::Graph{
            .nodes =
              std::vector{
                audio::flow::Node{
                  .id = "ao-source", .type = audio::flow::NodeType::Source, .name = "FLAC", .optFormat = cdFormat()},
                audio::flow::Node{
                  .id = "ao-sink", .type = audio::flow::NodeType::Sink, .name = "DAC", .optFormat = cdFormat()},
              },
            .connections =
              std::vector{
                audio::flow::Connection{.sourceId = "ao-source", .destId = "ao-sink"},
              },
          },
        .quality = audio::Quality::LinearIntervention,
        .qualityAssessments =
          std::vector{
            audio::NodeQualityAssessment{
              .nodeId = "ao-sink",
              .nodeName = "DAC",
              .nodeType = audio::flow::NodeType::Sink,
              .worstQuality = audio::Quality::LinearIntervention,
              .findings =
                std::vector{
                  audio::QualityFinding{.kind = audio::QualityFindingKind::BitPerfect},
                  audio::QualityFinding{.kind = audio::QualityFindingKind::SoftwareVolumeModification},
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
    CHECK(text.find("Linear intervention") != std::string::npos);
  }
} // namespace ao::tui::test

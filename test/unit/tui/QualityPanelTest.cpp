// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/QualityPanel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstdint>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    ftxui::Element englishQualityPanel(rt::PlaybackTransportSnapshot const& state, std::int32_t const columns = 0)
    {
      return qualityPanel(ao::test::englishMessageCatalog(), state, defaultKeymapPlan(), columns);
    }

    std::int32_t englishQualityPanelColumns(rt::PlaybackTransportSnapshot const& state,
                                            std::int32_t const terminalColumns)
    {
      return qualityPanelColumns(ao::test::englishMessageCatalog(), state, defaultKeymapPlan(), terminalColumns);
    }

    audio::SignalFormat cdFormat()
    {
      return audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16};
    }
  } // namespace

  TEST_CASE("QualityPanel - renders empty pipeline state", "[tui][unit][playback][quality]")
  {
    auto state = rt::PlaybackTransportSnapshot{.quality = rt::QualityState{.overall = audio::Quality::Unknown}};

    auto const text = renderText(englishQualityPanel(state), 96);

    CHECK_FALSE(text.contains("Quality"));
    CHECK_FALSE(text.contains("Audio Pipeline"));
    CHECK(text.contains("No audio pipeline yet"));
    CHECK_FALSE(text.contains("toggle"));
    CHECK(text.contains("Esc close"));
  }

  TEST_CASE("QualityPanel - renders selected device pipeline and findings", "[tui][unit][playback][quality]")
  {
    auto state =
      rt::PlaybackTransportSnapshot{
        .output =
          rt::OutputState{
            .selectedDevice = audio::OutputDeviceSelection{.backendId = audio::BackendId{"mock_backend"},
                                                           .deviceId = audio::DeviceId{"dac"}},
            .availableBackends =
              std::vector{
                rt::OutputBackendSnapshot{
                  .id = audio::BackendId{"mock_backend"},
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

    auto const text = renderText(englishQualityPanel(state), 96);

    CHECK(text.contains("Studio DAC"));
    CHECK_FALSE(text.contains("Quality"));
    CHECK(text.contains("[Source] FLAC"));
    CHECK(text.contains("44.1 kHz"));
    CHECK(text.contains("[Device] DAC"));
    CHECK(text.contains("Software volume attenuation"));
    CHECK(text.contains("Pipeline intervention"));
  }

  TEST_CASE("QualityPanel - localizes semantic copy and preserves external names",
            "[tui][unit][playback][quality][localization]")
  {
    auto const textCatalog = ao::test::messageCatalog("de-DE");
    auto state = rt::PlaybackTransportSnapshot{
      .output =
        rt::OutputState{
          .selectedDevice = audio::OutputDeviceSelection{.backendId = audio::BackendId{"mock_backend"},
                                                         .deviceId = audio::DeviceId{"dac"}},
          .availableBackends = {rt::OutputBackendSnapshot{
            .id = audio::BackendId{"mock_backend"},
            .devices = {rt::OutputDeviceSnapshot{.id = audio::DeviceId{"dac"}, .displayName = "Dvořák DAC"}},
          }},
        },
      .quality = rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                  .pipelineQuality = audio::Quality::LinearIntervention,
                                  .overall = audio::Quality::LinearIntervention,
                                  .assessments = {audio::NodeQualityAssessment{
                                    .nodeName = "誰か",
                                    .nodeType = audio::flow::NodeType::Source,
                                    .optFormat = cdFormat(),
                                    .findings = {audio::QualityFinding{
                                      .kind = audio::QualityFindingKind::Resampling,
                                      .quality = audio::Quality::LinearIntervention,
                                      .optFromFormat = cdFormat(),
                                      .optToFormat =
                                        audio::SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16},
                                    }},
                                  }}},
    };

    auto const text = renderText(qualityPanel(textCatalog, state, defaultKeymapPlan(), 0), 96);

    CHECK(text.contains("Dvořák DAC"));
    CHECK(text.contains("[Quelle] 誰か"));
    CHECK(text.contains("Neuabtastung: 44100 Hz → 48000 Hz"));
    CHECK(text.contains("Signalverarbeitung in der Audiokette"));

    state.quality.assessments.clear();
    auto const emptyText = renderText(qualityPanel(textCatalog, state, defaultKeymapPlan(), 0), 96);
    CHECK(emptyText.contains("Noch keine Audiokette"));
  }

  TEST_CASE("QualityPanel - width follows content and terminal bounds", "[tui][unit][playback][quality]")
  {
    auto state = rt::PlaybackTransportSnapshot{.quality = rt::QualityState{.overall = audio::Quality::Unknown}};
    auto const narrowColumns = englishQualityPanelColumns(state, 120);

    state.quality.assessments = std::vector{
      audio::NodeQualityAssessment{
        .nodeName = "Extremely Long Decoder Stage Name",
        .nodeType = audio::flow::NodeType::Source,
        .optFormat = cdFormat(),
      },
    };

    CHECK(englishQualityPanelColumns(state, 120) > narrowColumns);
    CHECK(englishQualityPanelColumns(state, 32) == 32);
  }

  TEST_CASE("QualityPanel - indicators use the Soul quality colors", "[tui][unit][playback][quality]")
  {
    CHECK(qualityIndicatorColor(uimodel::AudioQualityCategory::Medal) == uimodel::kAobusSoulRadiant);
    CHECK(qualityIndicatorColor(uimodel::AudioQualityCategory::Positive) == uimodel::kAobusSoulFlowing);
    CHECK(qualityIndicatorColor(uimodel::AudioQualityCategory::Diagnostic) == uimodel::kAobusSoulTurbulent);
    CHECK(qualityIndicatorColor(uimodel::AudioQualityCategory::Warning) == uimodel::kAobusSoulTurbulent);
    CHECK(qualityIndicatorColor(uimodel::AudioQualityCategory::Informational) == uimodel::kAobusSoulVeiled);
    CHECK(qualityIndicatorColor(uimodel::AudioQualityCategory::Unknown) == uimodel::kAobusSoulVeiled);
    CHECK(qualityIndicatorColor(uimodel::AudioQualityCategory::Clipped) == uimodel::kAobusSoulBurning);
  }
} // namespace ao::tui::test

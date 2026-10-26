// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/QualityPanel.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/tui/KeymapTestSupport.h"
#include "test/tui/RenderTestSupport.h"
#include "tui/TextCell.h"
#include <ao/audio/ChannelDeliveryEvidence.h>
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

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
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

    std::optional<ftxui::Box> findParagraphCells(ftxui::Screen const& screen, std::string_view const text)
    {
      // Include wide-glyph continuation cells instead of treating UTF-8 bytes as terminal columns.
      auto const glyphs = ftxui::Utf8ToGlyphs(text);

      if (glyphs.empty() || glyphs.size() > static_cast<std::size_t>(screen.dimx()))
      {
        return std::nullopt;
      }

      auto const columns = static_cast<std::int32_t>(glyphs.size());

      for (std::int32_t row = 0; row < screen.dimy(); ++row)
      {
        for (std::int32_t column = 0; column <= screen.dimx() - columns; ++column)
        {
          bool matches = true;

          for (std::int32_t offset = 0; offset < columns; ++offset)
          {
            auto const& expected = glyphs[static_cast<std::size_t>(offset)];
            auto const& actual = screen.PixelAt(column + offset, row).character;
            // Paragraph word separators may be unpainted cells; each still occupies one column.
            if (actual != expected && !(actual.empty() && expected == " "))
            {
              matches = false;
              break;
            }
          }

          if (matches)
          {
            return ftxui::Box{.x_min = column, .x_max = column + columns - 1, .y_min = row, .y_max = row};
          }
        }
      }

      return std::nullopt;
    }
  } // namespace

  TEST_CASE("QualityPanel - renders empty pipeline state", "[tui][unit][playback][quality]")
  {
    auto state = rt::PlaybackTransportSnapshot{.quality = rt::QualityState{.overall = audio::Quality::Unknown}};

    auto const text = renderText(englishQualityPanel(state), 96);
    auto const titleLine = std::string_view{text}.substr(0, text.find('\n'));

    // Titled like every other popover even before a device is selected.
    CHECK(titleLine.contains("─ Pipeline "));
    CHECK_FALSE(text.contains("Quality"));
    CHECK_FALSE(text.contains("Audio Pipeline"));
    CHECK(text.contains("No audio pipeline yet"));
    CHECK_FALSE(text.contains("Channel layout not identified"));
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
    auto const titleLine = std::string_view{text}.substr(0, text.find('\n'));

    // The device is the popover's context, not its title.
    CHECK(titleLine.contains("─ Pipeline ─ Studio DAC "));
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

  TEST_CASE("QualityPanel - renders layout uncertainty separately from the numerical headline",
            "[tui][unit][playback][quality]")
  {
    auto state = rt::PlaybackTransportSnapshot{
      .quality =
        rt::QualityState{
          .sourceQuality = audio::Quality::BitwisePerfect,
          .pipelineQuality = audio::Quality::BitwisePerfect,
          .overall = audio::Quality::BitwisePerfect,
          .assessments = {{.nodeType = audio::flow::NodeType::Source, .optFormat = cdFormat()}},
        },
    };

    auto const unknown = renderText(englishQualityPanel(state), 96);
    CHECK(unknown.contains("Bit-perfect PCM (observed software path)"));
    CHECK(unknown.contains("Channel layout not identified"));
    CHECK(unknown.contains("2 ch"));
    CHECK_FALSE(unknown.contains("Stereo"));

    state.quality.channelConfidence = audio::ChannelConfidence::Unverified;
    auto const unverified = renderText(englishQualityPanel(state), 96);
    CHECK(unverified.contains("Bit-perfect PCM (observed software path)"));
    CHECK(unverified.contains("Channel delivery not proved"));

    state.quality.channelConfidence = audio::ChannelConfidence::Inferred;
    CHECK(renderText(englishQualityPanel(state), 96).contains("Source channel layout inferred"));

    state.quality.channelConfidence = audio::ChannelConfidence::Verified;
    auto const verified = renderText(englishQualityPanel(state), 96);
    CHECK(verified.contains("Bit-perfect PCM (observed software path)"));
    CHECK_FALSE(verified.contains("Channel layout not identified"));
    CHECK_FALSE(verified.contains("Source channel layout inferred"));
    CHECK_FALSE(verified.contains("Channel delivery not proved"));
  }

  TEST_CASE("QualityPanel - narrow terminal cells retain wrapped headlines and independent notes",
            "[tui][unit][playback][quality][localization]")
  {
    struct Case final
    {
      std::string_view locale;
      audio::ChannelConfidence confidence;
      std::vector<std::string_view> headlineChunks;
      std::vector<std::string_view> noteChunks;
      bool noteWraps = false;
    };

    auto const cases = std::vector<Case>{
      {.locale = "en-US",
       .confidence = audio::ChannelConfidence::Unknown,
       .headlineChunks = {"Bit-perfect PCM", "(observed", "software", "path)"},
       .noteChunks = {"Channel layout", "not", "identified"},
       .noteWraps = true},
      {.locale = "en-US",
       .confidence = audio::ChannelConfidence::Unverified,
       .headlineChunks = {"Bit-perfect PCM", "(observed", "software", "path)"},
       .noteChunks = {"Channel delivery", "not", "proved"}},
      {.locale = "zh-CN",
       .confidence = audio::ChannelConfidence::Unknown,
       .headlineChunks = {"Bit-Perfect", "PCM", "（已观测的软件链路）"},
       .noteChunks = {"声道布局未识别"}},
      {.locale = "zh-CN",
       .confidence = audio::ChannelConfidence::Unverified,
       .headlineChunks = {"Bit-Perfect", "PCM", "（已观测的软件链路）"},
       .noteChunks = {"声道传递尚未证实"}},
    };
    // A 32-cell constrained popup has room for its border and body gutters.
    // The English Unknown note must wrap; the shorter CJK notes fit in cells,
    // not UTF-8 bytes. The longer numerical headline must wrap in both locales.
    constexpr std::int32_t kNarrowColumns = 32;
    constexpr std::int32_t kWideColumns = 96;

    for (auto const& testCase : cases)
    {
      CAPTURE(testCase.locale, testCase.confidence);
      auto const catalog = ao::test::messageCatalog(testCase.locale);
      auto const state = rt::PlaybackTransportSnapshot{
        .quality = {.sourceQuality = audio::Quality::BitwisePerfect,
                    .pipelineQuality = audio::Quality::BitwisePerfect,
                    .overall = audio::Quality::BitwisePerfect,
                    .channelConfidence = testCase.confidence,
                    .assessments = {{.nodeType = audio::flow::NodeType::Source}}},
      };
      auto const presentation = uimodel::AudioQualityFormatter{catalog}.presentation(state.quality);
      auto const wide = renderElementFit(qualityPanel(catalog, state, defaultKeymapPlan(), kWideColumns), kWideColumns);
      INFO(wide.text);
      REQUIRE(findParagraphCells(wide.screen, presentation.headline));
      REQUIRE(findParagraphCells(wide.screen, presentation.channelNote));
      auto const wideRows = wide.screen.dimy();
      auto const narrow =
        renderElementFit(qualityPanel(catalog, state, defaultKeymapPlan(), kNarrowColumns), kNarrowColumns);
      INFO(narrow.text);
      CHECK(narrow.screen.dimx() == kNarrowColumns);
      CHECK(narrow.screen.dimy() > wideRows);
      CHECK_FALSE(findParagraphCells(narrow.screen, presentation.headline));
      CHECK(findParagraphCells(narrow.screen, presentation.channelNote).has_value() == !testCase.noteWraps);
      std::int32_t lastHeadlineRow = -1;

      for (bool const isNote : {false, true})
      {
        auto const& chunks = isNote ? testCase.noteChunks : testCase.headlineChunks;
        std::int32_t firstRow = -1;
        std::int32_t previousRow = -1;
        std::int32_t previousColumn = -1;

        for (auto const chunk : chunks)
        {
          CAPTURE(chunk);
          // Inspect actual screen glyphs, including CJK continuation cells,
          // rather than joining or slicing flattened rendered lines.
          auto const optCells = findParagraphCells(narrow.screen, chunk);
          REQUIRE(optCells);
          CHECK(optCells->x_min >= 2);
          CHECK(optCells->x_min + cellWidth(chunk) <= kNarrowColumns - 2);
          CHECK(
            (optCells->y_min > previousRow || (optCells->y_min == previousRow && optCells->x_min > previousColumn)));
          CHECK(narrow.screen.PixelAt(optCells->x_min, optCells->y_min).dim == isNote);

          if (firstRow < 0)
          {
            firstRow = optCells->y_min;
          }

          previousRow = optCells->y_min;
          previousColumn = optCells->x_min;
        }

        if (isNote)
        {
          CHECK(firstRow > lastHeadlineRow);
          CHECK((previousRow > firstRow) == testCase.noteWraps);
        }
        else
        {
          CHECK(previousRow > firstRow);
          lastHeadlineRow = previousRow;
        }
      }
    }
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

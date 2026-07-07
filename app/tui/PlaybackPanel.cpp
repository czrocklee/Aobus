// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackPanel.h"

#include "Model.h"
#include "ShellModel.h"
#include "TextCell.h"
#include <ao/audio/Backend.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kOutputDeviceRows = 14;
    constexpr std::int32_t kOutputDeviceInnerColumns = kOutputDevicePanelColumns - 2;

    ftxui::Element qualityDot(uimodel::AudioQualityCategory const category)
    {
      auto const style = qualityIndicatorStyle(category);
      return ftxui::text("●") | ftxui::color(ftxui::Color::RGB(style.red, style.green, style.blue));
    }

    std::string outputSummary(uimodel::OutputDeviceViewState const* const outputView)
    {
      if (outputView == nullptr || outputView->outputBackendSummary.empty())
      {
        return "--";
      }

      return outputView->outputBackendSummary;
    }

    ftxui::Element outputBadge(uimodel::OutputDeviceViewState const* const outputView)
    {
      using namespace ftxui;

      auto elementPtr = text(" " + outputSummary(outputView) + " ");

      if (outputView == nullptr || !outputView->hasActiveOutputDevice)
      {
        return elementPtr | dim;
      }

      return elementPtr | bold;
    }

    std::string outputDeviceFooter(uimodel::OutputDeviceViewState const& view)
    {
      if (!view.outputDeviceStatus.empty())
      {
        return view.outputDeviceStatus;
      }

      return "No output device selected";
    }

    ftxui::Element outputText(std::string value, bool const dimmed = false)
    {
      auto elementPtr = ftxui::text(fitCellText(value, kOutputDeviceInnerColumns)) |
                        ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kOutputDeviceInnerColumns);

      if (dimmed)
      {
        elementPtr = elementPtr | ftxui::dim;
      }

      return elementPtr;
    }
  } // namespace

  ftxui::Element playbackBar(PlaybackBarViewState const& view)
  {
    using namespace ftxui;

    auto fallbackState = rt::PlaybackState{};
    auto const& state = view.playbackState == nullptr ? fallbackState : *view.playbackState;
    auto const presentation = uimodel::audioQualityPresentation(state.quality);
    auto const quality = presentation.headline.empty() ? qualityIndicatorStyle(state.quality.overall)
                                                       : qualityIndicatorStyle(presentation.category);
    auto const title = state.nowPlaying.title.empty() ? std::string{"No active track"} : state.nowPlaying.title;
    auto const artist = state.nowPlaying.artist.empty() ? std::string{"-"} : state.nowPlaying.artist;
    auto const elapsed = formatDuration(view.displayElapsed);
    auto const duration = state.duration.count() > 0 ? formatDuration(state.duration) : std::string{"--:--"};
    auto const volume = std::format("{}%", static_cast<std::int32_t>(std::round(state.volume.level * 100.0F)));
    auto libraryElementPtr = text("  " + std::string{view.listTitle}) | dim;
    auto presentationElementPtr = text("  " + presentationBadgeLabel(view.presentationId)) | dim;
    auto qualityElementPtr = text("  ●") | color(Color::RGB(quality.red, quality.green, quality.blue));
    auto outputElementPtr = outputBadge(view.outputView);

    if (view.libraryBox != nullptr)
    {
      libraryElementPtr = std::move(libraryElementPtr) | reflect(*view.libraryBox);
    }

    if (view.qualityBox != nullptr)
    {
      qualityElementPtr = std::move(qualityElementPtr) | reflect(*view.qualityBox);
    }

    if (view.presentationBox != nullptr)
    {
      presentationElementPtr = std::move(presentationElementPtr) | reflect(*view.presentationBox);
    }

    if (view.outputDeviceBox != nullptr)
    {
      outputElementPtr = std::move(outputElementPtr) | reflect(*view.outputDeviceBox);
    }

    return hbox({
      text("Aobus") | bold,
      std::move(libraryElementPtr),
      std::move(presentationElementPtr),
      std::move(qualityElementPtr),
      text(" "),
      std::move(outputElementPtr),
      text("  " + title) | bold | flex,
      text(artist) | dim,
      text("  " + transportLabel(state.transport)) | dim,
      text("  " + elapsed + " / " + duration),
      text("  "),
      text(volume),
    });
  }

  ftxui::Element qualityPanel(rt::PlaybackState const& state)
  {
    using namespace ftxui;

    auto rows = Elements{};
    auto deviceName = std::string{};

    for (auto const& backend : state.output.availableBackends)
    {
      for (auto const& device : backend.devices)
      {
        if (device.id == state.output.selectedDevice.deviceId)
        {
          deviceName = device.displayName;
          break;
        }
      }

      if (!deviceName.empty())
      {
        break;
      }
    }

    rows.push_back(hbox({
      text(deviceName.empty() ? "Audio Pipeline" : deviceName) | bold | flex,
      qualityDot(uimodel::audioQualityPresentation(state.quality).category),
    }));
    rows.push_back(separator());

    if (state.quality.assessments.empty())
    {
      rows.push_back(text("No audio pipeline yet") | dim);
    }

    for (auto const& assessment : state.quality.assessments)
    {
      auto nodeLine = uimodel::audioNodeTypeLabel(assessment.nodeType);

      if (!assessment.nodeName.empty())
      {
        nodeLine.append(" ");
        nodeLine.append(assessment.nodeName);
      }

      if (assessment.optFormat)
      {
        auto const preferValidBits = assessment.nodeType == audio::flow::NodeType::Source;
        nodeLine.append(" (");
        nodeLine.append(uimodel::audioFormatLabel(*assessment.optFormat, preferValidBits));
        nodeLine.push_back(')');
      }

      rows.push_back(text(nodeLine));

      for (auto const& finding : assessment.findings)
      {
        auto const findingText = uimodel::audioFindingLabel(finding);

        if (findingText.empty())
        {
          continue;
        }

        rows.push_back(hbox({
          text("  "),
          qualityDot(uimodel::audioFindingCategory(finding)),
          text(" " + findingText) | dim,
        }));
      }
    }

    auto const presentation = uimodel::audioQualityPresentation(state.quality);

    if (!presentation.headline.empty())
    {
      rows.push_back(separator());
      rows.push_back(hbox({
        qualityDot(presentation.category),
        text(" " + presentation.headline),
      }));
    }

    rows.push_back(separator());
    rows.push_back(text(std::string{overlayHint(Overlay::QualityPanel)}) | dim);

    return vbox(std::move(rows)) | border | size(WIDTH, EQUAL, kQualityPanelColumns);
  }

  ftxui::Element outputDevicePanel(uimodel::OutputDeviceViewState const& view,
                                   std::int32_t const selectedRow,
                                   std::vector<OutputDeviceRowBox>* const rowBoxes)
  {
    using namespace ftxui;

    auto rows = Elements{};
    auto listRows = Elements{};
    std::int32_t focusRow = 0;

    if (rowBoxes != nullptr)
    {
      rowBoxes->clear();
      rowBoxes->reserve(view.rows.size());
    }

    rows.push_back(hbox({
      text("Output Devices") | bold | flex,
      text(outputSummary(&view)) | bold,
    }));
    rows.push_back(separator());

    if (view.rows.empty())
    {
      listRows.push_back(outputText("No output devices found", true));
    }

    for (std::size_t index = 0; index < view.rows.size(); ++index)
    {
      auto const& row = view.rows[index];

      if (row.kind == uimodel::OutputDeviceRow::Kind::BackendHeader)
      {
        listRows.push_back(outputText(row.title, true));
        continue;
      }

      auto titleLine = std::string{row.isActive ? "* " : "  "} + row.title;
      auto rowPtr = outputText(std::move(titleLine));

      if (std::cmp_equal(index, selectedRow))
      {
        focusRow = static_cast<std::int32_t>(listRows.size());
        rowPtr = rowPtr | inverted | bold;
      }

      if (rowBoxes != nullptr)
      {
        rowBoxes->push_back(OutputDeviceRowBox{.rowIndex = static_cast<std::int32_t>(index)});
        rowPtr = std::move(rowPtr) | reflect(rowBoxes->back().box);
      }

      listRows.push_back(std::move(rowPtr));

      if (!row.description.empty())
      {
        auto descPtr = outputText("  " + row.description, true);

        if (rowBoxes != nullptr)
        {
          descPtr = std::move(descPtr) | reflect(rowBoxes->back().secondaryBox);
        }

        listRows.push_back(std::move(descPtr));
      }
    }

    rows.push_back(vbox(std::move(listRows)) | focusPosition(0, focusRow) | vscroll_indicator | frame |
                   size(HEIGHT, EQUAL, kOutputDeviceRows));
    rows.push_back(separator());
    rows.push_back(outputText(outputDeviceFooter(view), true));
    rows.push_back(outputText(std::string{overlayHint(Overlay::OutputDevices)}, true));

    return vbox(std::move(rows)) | border | size(WIDTH, EQUAL, kOutputDevicePanelColumns);
  }
} // namespace ao::tui

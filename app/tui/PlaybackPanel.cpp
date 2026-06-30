// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackPanel.h"

#include "Model.h"
#include <ao/audio/Backend.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
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

    ftxui::Element qualityDot(audio::Quality const quality)
    {
      auto const style = qualityIndicatorStyle(quality);
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

    std::string truncateToCellWidth(std::string_view const value, std::int32_t const width)
    {
      auto result = std::string{};
      std::int32_t used = 0;

      for (auto const& glyph : ftxui::Utf8ToGlyphs(std::string{value}))
      {
        auto const glyphWidth = static_cast<std::int32_t>(ftxui::string_width(glyph));

        if (glyphWidth == 0)
        {
          result += glyph;
          continue;
        }

        if (used + glyphWidth > width)
        {
          break;
        }

        result += glyph;
        used += glyphWidth;
      }

      return result;
    }

    std::string fitCellText(std::string value, std::int32_t const width)
    {
      value = truncateToCellWidth(value, width);
      auto const padding = width - static_cast<std::int32_t>(ftxui::string_width(value));

      if (padding > 0)
      {
        value.append(static_cast<std::size_t>(padding), ' ');
      }

      return value;
    }

    ftxui::Element outputText(std::string value, bool const dimmed = false)
    {
      auto elementPtr = ftxui::text(fitCellText(std::move(value), kOutputDeviceInnerColumns)) |
                        ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kOutputDeviceInnerColumns);

      if (dimmed)
      {
        elementPtr = elementPtr | ftxui::dim;
      }

      return elementPtr;
    }
  } // namespace

  ftxui::Element playbackBar(rt::PlaybackState const& state,
                             std::string const& listTitle,
                             std::chrono::milliseconds const displayElapsed,
                             uimodel::OutputDeviceViewState const* const outputView,
                             ftxui::Box* const outputDeviceBox,
                             ftxui::Box* const libraryBox,
                             ftxui::Box* const qualityBox)
  {
    using namespace ftxui;

    auto const quality = qualityIndicatorStyle(state.quality);
    auto const title = state.trackTitle.empty() ? std::string{"No active track"} : state.trackTitle;
    auto const artist = state.trackArtist.empty() ? std::string{"-"} : state.trackArtist;
    auto const elapsed = formatDuration(displayElapsed);
    auto const duration = state.duration.count() > 0 ? formatDuration(state.duration) : std::string{"--:--"};
    auto const volume = std::format("{}%", static_cast<std::int32_t>(std::round(state.volume * 100.0F)));
    auto libraryElementPtr = text("  " + listTitle) | dim;
    auto qualityElementPtr = text("  ●") | color(Color::RGB(quality.red, quality.green, quality.blue));
    auto outputElementPtr = outputBadge(outputView);

    if (libraryBox != nullptr)
    {
      libraryElementPtr = std::move(libraryElementPtr) | reflect(*libraryBox);
    }

    if (qualityBox != nullptr)
    {
      qualityElementPtr = std::move(qualityElementPtr) | reflect(*qualityBox);
    }

    if (outputDeviceBox != nullptr)
    {
      outputElementPtr = std::move(outputElementPtr) | reflect(*outputDeviceBox);
    }

    return hbox({
      text("Aobus") | bold,
      std::move(libraryElementPtr),
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

    for (auto const& backend : state.availableOutputBackends)
    {
      for (auto const& device : backend.devices)
      {
        if (device.id == state.selectedOutputDevice.deviceId)
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
      qualityDot(state.quality),
    }));
    rows.push_back(separator());

    auto const path = uimodel::playbackPath(state.flow);

    if (path.empty())
    {
      rows.push_back(text("No audio pipeline yet") | dim);
    }

    for (auto const* node : path)
    {
      auto nodeLine = uimodel::audioNodeTypeLabel(node->type);

      if (!node->name.empty())
      {
        nodeLine.append(" ");
        nodeLine.append(node->name);
      }

      if (node->optFormat)
      {
        auto const preferValidBits = node->type == audio::flow::NodeType::Source;
        nodeLine.append(" (");
        nodeLine.append(uimodel::audioFormatLabel(*node->optFormat, preferValidBits));
        nodeLine.push_back(')');
      }

      rows.push_back(text(nodeLine));

      auto const assessmentIt =
        std::ranges::find(state.qualityAssessments, node->id, &audio::NodeQualityAssessment::nodeId);

      if (assessmentIt == state.qualityAssessments.end())
      {
        continue;
      }

      for (auto const& finding : assessmentIt->findings)
      {
        auto const findingText = uimodel::audioFindingLabel(finding);

        if (findingText.empty())
        {
          continue;
        }

        rows.push_back(hbox({
          text("  "),
          qualityDot(uimodel::qualityForFinding(finding)),
          text(" " + findingText) | dim,
        }));
      }
    }

    auto const conclusion = uimodel::audioQualityConclusion(state.quality);

    if (!conclusion.empty())
    {
      rows.push_back(separator());
      rows.push_back(hbox({
        qualityDot(state.quality),
        text(" " + conclusion),
      }));
    }

    rows.push_back(separator());
    rows.push_back(text("a toggle  Esc close") | dim);

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
    rows.push_back(outputText("o toggle  Enter select  Esc close", true));

    return vbox(std::move(rows)) | border | size(WIDTH, EQUAL, kOutputDevicePanelColumns);
  }
} // namespace ao::tui

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "QualityPanel.h"

#include "QualityIndicatorStyle.h"
#include "ShellInteractionModel.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace ao::tui
{
  namespace
  {
    ftxui::Element qualityDot(uimodel::AudioQualityCategory const category)
    {
      auto const style = qualityIndicatorStyle(category);
      return ftxui::text("●") | ftxui::color(ftxui::Color::RGB(style.red, style.green, style.blue));
    }

    std::string selectedDeviceName(rt::PlaybackState const& state)
    {
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

      return deviceName;
    }

    std::string qualityNodeLine(audio::NodeQualityAssessment const& assessment)
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

      return nodeLine;
    }
  } // namespace

  std::int32_t qualityPanelColumns(rt::PlaybackState const& state, std::int32_t const terminalColumns)
  {
    auto const deviceName = selectedDeviceName(state);
    auto contentColumns = std::max(cellWidth(deviceName), cellWidth(overlayHint(Overlay::QualityPanel)));

    if (state.quality.assessments.empty())
    {
      contentColumns = std::max(contentColumns, cellWidth("No audio pipeline yet"));
    }

    for (auto const& assessment : state.quality.assessments)
    {
      contentColumns = std::max(contentColumns, cellWidth(qualityNodeLine(assessment)));

      for (auto const& finding : assessment.findings)
      {
        if (auto const findingText = uimodel::audioFindingLabel(finding); !findingText.empty())
        {
          contentColumns = std::max(contentColumns, cellWidth("  ● ") + cellWidth(findingText));
        }
      }
    }

    auto const presentation = uimodel::audioQualityPresentation(state.quality);

    if (!presentation.headline.empty())
    {
      contentColumns = std::max(contentColumns, cellWidth("● ") + cellWidth(presentation.headline));
    }

    return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element qualityPanel(rt::PlaybackState const& state, std::int32_t columns)
  {
    using namespace ftxui;

    if (columns <= 0)
    {
      columns = qualityPanelColumns(state, 0);
    }

    auto rows = Elements{};
    auto const deviceName = selectedDeviceName(state);

    if (state.quality.assessments.empty())
    {
      rows.push_back(text("No audio pipeline yet") | dim);
    }

    for (auto const& assessment : state.quality.assessments)
    {
      rows.push_back(text(qualityNodeLine(assessment)));

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
    rows.push_back(style::panelFooterHint(overlayHint(Overlay::QualityPanel)));

    return style::popupPanel(deviceName, vbox(std::move(rows))) | size(WIDTH, EQUAL, columns);
  }
} // namespace ao::tui

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "QualityPanel.h"

#include "ShellInteractionModel.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/audio/QualityAnalyzer.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#ifdef RGB
#undef RGB
#endif

namespace ao::tui
{
  uimodel::AobusSoulRgb qualityIndicatorColor(uimodel::AudioQualityCategory const category)
  {
    switch (category)
    {
      case uimodel::AudioQualityCategory::Medal: return uimodel::kAobusSoulRadiant;
      case uimodel::AudioQualityCategory::Positive: return uimodel::kAobusSoulFlowing;
      case uimodel::AudioQualityCategory::Diagnostic:
      case uimodel::AudioQualityCategory::Warning: return uimodel::kAobusSoulTurbulent;
      case uimodel::AudioQualityCategory::Clipped: return uimodel::kAobusSoulBurning;
      case uimodel::AudioQualityCategory::Informational:
      case uimodel::AudioQualityCategory::Unknown: return uimodel::kAobusSoulVeiled;
    }

    return uimodel::kAobusSoulVeiled;
  }

  namespace
  {
    ftxui::Element qualityDot(uimodel::AudioQualityCategory const category)
    {
      auto const color = qualityIndicatorColor(category);
      return ftxui::text("●") | ftxui::color(ftxui::Color::RGB(color.red, color.green, color.blue));
    }

    std::string selectedDeviceName(rt::PlaybackTransportSnapshot const& state)
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

    std::string qualityNodeLine(uimodel::AudioQualityFormatter const& formatter,
                                audio::NodeQualityAssessment const& assessment)
    {
      auto nodeLine = formatter.nodeTypeLabel(assessment.nodeType);

      if (!assessment.nodeName.empty())
      {
        nodeLine.append(" ");
        nodeLine.append(assessment.nodeName);
      }

      if (assessment.optFormat)
      {
        nodeLine.append(" (");
        nodeLine.append(formatter.formatLabel(*assessment.optFormat));
        nodeLine.push_back(')');
      }

      return nodeLine;
    }
  } // namespace

  std::int32_t qualityPanelColumns(i18n::MessageCatalog const& textCatalog,
                                   rt::PlaybackTransportSnapshot const& state,
                                   TuiKeymapPlan const& keymapPlan,
                                   std::int32_t const terminalColumns)
  {
    auto const deviceName = selectedDeviceName(state);
    auto contentColumns =
      std::max(cellWidth(deviceName), cellWidth(overlayHint(textCatalog, keymapPlan, Overlay::QualityPanel)));

    if (state.quality.assessments.empty())
    {
      contentColumns = std::max(
        contentColumns, cellWidth(i18n::requiredText(textCatalog, i18n::MessageId::TuiPlaybackNoAudioPipeline)));
    }

    auto const formatter = uimodel::AudioQualityFormatter{textCatalog};

    for (auto const& assessment : state.quality.assessments)
    {
      contentColumns = std::max(contentColumns, cellWidth(qualityNodeLine(formatter, assessment)));

      for (auto const& finding : assessment.findings)
      {
        if (auto const findingText = formatter.findingLabel(finding); !findingText.empty())
        {
          contentColumns = std::max(contentColumns, cellWidth("  ● ") + cellWidth(findingText));
        }
      }
    }

    auto const presentation = formatter.presentation(state.quality);

    if (!presentation.headline.empty())
    {
      contentColumns = std::max(contentColumns, cellWidth("● ") + cellWidth(presentation.headline));
    }

    return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element qualityPanel(i18n::MessageCatalog const& textCatalog,
                              rt::PlaybackTransportSnapshot const& state,
                              TuiKeymapPlan const& keymapPlan,
                              std::int32_t columns)
  {
    using namespace ftxui;

    if (columns <= 0)
    {
      columns = qualityPanelColumns(textCatalog, state, keymapPlan, 0);
    }

    auto rows = Elements{};
    auto const deviceName = selectedDeviceName(state);

    if (state.quality.assessments.empty())
    {
      rows.push_back(text(std::string{i18n::requiredText(textCatalog, i18n::MessageId::TuiPlaybackNoAudioPipeline)}) |
                     dim);
    }

    auto const formatter = uimodel::AudioQualityFormatter{textCatalog};

    for (auto const& assessment : state.quality.assessments)
    {
      rows.push_back(text(qualityNodeLine(formatter, assessment)));

      for (auto const& finding : assessment.findings)
      {
        auto const findingText = formatter.findingLabel(finding);

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

    auto const presentation = formatter.presentation(state.quality);

    if (!presentation.headline.empty())
    {
      rows.push_back(separator());
      rows.push_back(hbox({
        qualityDot(presentation.category),
        text(" " + presentation.headline),
      }));
    }

    rows.push_back(separator());
    rows.push_back(style::panelFooterHint(overlayHint(textCatalog, keymapPlan, Overlay::QualityPanel)));

    return style::popupPanel(deviceName, vbox(std::move(rows))) | size(WIDTH, EQUAL, columns);
  }
} // namespace ao::tui

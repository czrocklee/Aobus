// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackPanel.h"

#include "Model.h"
#include <ao/audio/Backend.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kQualityPanelColumns = 48;

    ftxui::Element qualityDot(audio::Quality const quality)
    {
      auto const style = qualityIndicatorStyle(quality);
      return ftxui::text("●") | ftxui::color(ftxui::Color::RGB(style.red, style.green, style.blue));
    }
  } // namespace

  ftxui::Element playbackBar(rt::PlaybackState const& state,
                             std::string const& listTitle,
                             std::chrono::milliseconds const displayElapsed)
  {
    using namespace ftxui;

    auto const quality = qualityIndicatorStyle(state.quality);
    auto const title = state.trackTitle.empty() ? std::string{"No active track"} : state.trackTitle;
    auto const artist = state.trackArtist.empty() ? std::string{"-"} : state.trackArtist;
    auto const elapsed = formatDuration(displayElapsed);
    auto const duration = state.duration.count() > 0 ? formatDuration(state.duration) : std::string{"--:--"};
    auto const volume = std::format("{}%", static_cast<std::int32_t>(std::round(state.volume * 100.0F)));

    return hbox({
      text("Aobus") | bold,
      text("  " + listTitle) | dim,
      text("  ●") | color(Color::RGB(quality.red, quality.green, quality.blue)),
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
} // namespace ao::tui

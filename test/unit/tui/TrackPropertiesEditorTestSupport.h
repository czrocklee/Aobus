// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "tui/TrackPropertiesEditor.h"

#include <ftxui/component/event.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  constexpr std::int32_t kTerminalColumns = 80;
  constexpr std::int32_t kTerminalRows = 24;

  struct TrackFixture final
  {
    std::string title;
    std::string album;
    std::uint16_t year = 0;
    std::string codec = "FLAC";
  };

  TrackPropertiesEditor makeEditor(std::vector<TrackFixture> tracks,
                                   TrackPropertiesEditor::CompletionProvider completionProvider = {},
                                   std::vector<std::pair<std::string, std::size_t>> tagCounts = {},
                                   std::vector<std::string> tagSuggestions = {},
                                   std::string_view locale = "en",
                                   TrackEditorMode mode = TrackEditorMode::Properties);
  std::string frame(TrackPropertiesEditor const& editor);
  ftxui::Event applyEvent();
  ftxui::Event reloadEvent();
  ftxui::Event clearEvent();
  ftxui::Event restoreEvent();
  ftxui::Event completeEvent();
  void focusRow(TrackPropertiesEditor& editor, std::string_view label);
  void selectTab(TrackPropertiesEditor& editor, TrackEditorTab tab);
  void typeText(TrackPropertiesEditor& editor, std::string_view text);
  bool hasCaretOnLine(ftxui::Screen const& screen, std::int32_t line);
} // namespace ao::tui::test

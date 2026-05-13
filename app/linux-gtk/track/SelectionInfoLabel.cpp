// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/SelectionInfoLabel.h"
#include <runtime/AppSession.h>
#include <runtime/ViewService.h>

#include <format>

namespace ao::gtk
{
  namespace
  {
    std::string formatDuration(std::chrono::milliseconds ms)
    {
      auto const totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(ms).count();
      auto const hours = totalSeconds / 3600;
      auto const minutes = (totalSeconds % 3600) / 60;
      auto const seconds = totalSeconds % 60;

      if (hours > 0)
      {
        return std::format("{}:{:02}:{:02}", hours, minutes, seconds);
      }
      return std::format("{}:{:02}", minutes, seconds);
    }
  }

  SelectionInfoLabel::SelectionInfoLabel(rt::ViewService& viewService)
    : _viewService{viewService}
  {
    _label.add_css_class("dim-label");
    _label.set_halign(Gtk::Align::END);

    _selectionChangedSub =
      _viewService.onSelectionChanged([this](auto const& ev) { updateState(ev.selection.size()); });

    updateState(0);
  }

  SelectionInfoLabel::~SelectionInfoLabel() = default;

  void SelectionInfoLabel::updateState(std::size_t count, std::optional<std::chrono::milliseconds> totalDuration)
  {
    if (count == 0)
    {
      _label.set_text("");
      return;
    }

    auto text = std::format("{} {}", count, count == 1 ? "item selected" : "items selected");

    if (totalDuration && totalDuration->count() > 0)
    {
      text += std::format(" ({})", formatDuration(*totalDuration));
    }

    _label.set_text(text);
  }
} // namespace ao::gtk

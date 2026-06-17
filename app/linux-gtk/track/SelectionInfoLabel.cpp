// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/SelectionInfoLabel.h"

#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewService.h>
#include <ao/uimodel/track/SelectionSummary.h>

#include <gtkmm/enums.h>

#include <chrono>
#include <cstddef>
#include <optional>

namespace ao::gtk
{
  SelectionInfoLabel::SelectionInfoLabel(rt::ViewService& viewService)
    : _viewService{viewService}
  {
    _label.add_css_class("dim-label");
    _label.set_halign(Gtk::Align::END);

    _selectionChangedSub = _viewService.onSelectionChanged(
      [this](auto const& ev) { updateState(ev.selection.size(), _viewService.selectionDuration(ev.viewId)); });

    updateState(0);
  }

  SelectionInfoLabel::~SelectionInfoLabel() = default;

  void SelectionInfoLabel::updateState(std::size_t count, std::optional<std::chrono::milliseconds> optTotalDuration)
  {
    _label.set_text(uimodel::track::selectionSummaryText(count, optTotalDuration));
  }
} // namespace ao::gtk

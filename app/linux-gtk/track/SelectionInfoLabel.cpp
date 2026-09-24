// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/SelectionInfoLabel.h"

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/uimodel/library/track/TrackSelectionSummary.h>

#include <gtkmm/enums.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>

namespace ao::gtk
{
  SelectionInfoLabel::SelectionInfoLabel(rt::ViewService& viewService,
                                         rt::WorkspaceService& workspace,
                                         i18n::MessageCatalog textCatalog)
    : _viewService{viewService}, _workspace{workspace}, _textCatalog{std::move(textCatalog)}
  {
    _label.add_css_class("dim-label");
    _label.set_halign(Gtk::Align::END);

    _selectionChangedSub = _viewService.onSelectionChanged(
      [this](auto const& ev)
      {
        if (ev.viewId == _activeViewId)
        {
          updateState(ev.selection.size(), _viewService.selectionDuration(ev.viewId));
        }
      });
    _workspaceChangedSub = _workspace.onChanged([this](rt::WorkspaceChanged const& changed)
                                                { refreshActiveView(changed.snapshot.activeViewId); });

    // Reconcile after both observers are live so construction cannot leave a
    // pre-existing active selection blank.
    refreshActiveView(_workspace.snapshot().activeViewId);
  }

  SelectionInfoLabel::~SelectionInfoLabel() = default;

  void SelectionInfoLabel::refreshActiveView(rt::ViewId const activeViewId)
  {
    _activeViewId = activeViewId;

    if (_activeViewId == rt::kInvalidViewId)
    {
      updateState(0);
      return;
    }

    auto const stateRes = _viewService.findTrackListState(_activeViewId);

    if (!stateRes)
    {
      // A queued workspace change may name a view already retired by a newer
      // commit. The next workspace event reconciles the current identity.
      updateState(0);
      return;
    }

    updateState(stateRes->selection.size(), _viewService.selectionDuration(_activeViewId));
  }

  void SelectionInfoLabel::updateState(std::size_t count, std::optional<std::chrono::milliseconds> optTotalDuration)
  {
    _label.set_text(uimodel::trackSelectionSummaryText(_textCatalog, count, optTotalDuration));
  }
} // namespace ao::gtk

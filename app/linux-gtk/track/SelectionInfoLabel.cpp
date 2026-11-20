// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/SelectionInfoLabel.h"

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/LibraryChanges.h>
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
                                         rt::LibraryChanges const& changes,
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
    _workspaceChangedSub = _workspace.onChanged(
      [this](rt::WorkspaceChanged const& changed)
      {
        if (changed.snapshot.activeViewId != _activeViewId)
        {
          refreshActiveView(changed.snapshot.activeViewId);
        }
      });
    _libraryChangedSub =
      changes.onChanged([this](rt::LibraryChangeSet const& changed) { refreshActiveView(_activeViewId, &changed); });

    // Reconcile after the observers are live so construction cannot leave a
    // pre-existing active selection blank.
    refreshActiveView(_workspace.snapshot().activeViewId);
  }

  SelectionInfoLabel::~SelectionInfoLabel() = default;

  void SelectionInfoLabel::refreshActiveView(rt::ViewId const activeViewId, rt::LibraryChangeSet const* const changes)
  {
    _activeViewId = activeViewId;

    // A queued workspace notification may name an already retired view, whose selection is empty.
    auto const selection = _viewService.selectedTrackIds(activeViewId);

    if (changes == nullptr || uimodel::shouldRefreshTrackSelectionSummary(*changes, selection))
    {
      updateState(selection.size(), _viewService.selectionDuration(activeViewId));
    }
  }

  void SelectionInfoLabel::updateState(std::size_t count, std::optional<std::chrono::milliseconds> optTotalDuration)
  {
    _label.set_text(uimodel::trackSelectionSummaryText(_textCatalog, count, optTotalDuration));
  }
} // namespace ao::gtk

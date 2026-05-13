// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackPresentation.h"
#include "track/TrackRowObject.h"

#include <gtkmm/signallistitemfactory.h>

#include <functional>
#include <string>

namespace ao::gtk
{
  /**
   * Callback for committing inline metadata edits from the UI.
   */
  using MetadataCommitFn =
    std::function<void(Glib::RefPtr<TrackRowObject> const& row, TrackColumn column, std::string newValue)>;

  /**
   * Builds a SignalListItemFactory for the given column definition.
   *
   * The returned factory handles the full lifecycle:
   * - setup: creates the appropriate widget hierarchy (label, stack, etc.)
   * - bind: connects the row object data and establishes reactive updates (playing state, metadata)
   * - unbind: performs necessary cleanup to avoid leaks when rows are recycled
   *
   * @param definition The definition of the column to build.
   * @param commitFn A callback to handle metadata changes if the column is editable.
   */
  Glib::RefPtr<Gtk::SignalListItemFactory> buildColumnFactory(TrackColumnDefinition const& definition,
                                                              MetadataCommitFn const& commitFn);
} // namespace ao::gtk

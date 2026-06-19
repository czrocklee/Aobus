// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackRowObject.h"
#include <ao/rt/TrackField.h>

#include <glibmm/refptr.h>
#include <gtkmm/signallistitemfactory.h>

#include <functional>
#include <string>

namespace ao::gtk
{
  class TrackListModel;

  /**
   * Callback for committing inline metadata edits from the UI.
   */
  using MetadataCommitFn =
    std::function<void(Glib::RefPtr<TrackRowObject> const& row, rt::TrackField field, std::string newValue)>;

  /**
   * Builds a SignalListItemFactory for the given track field.
   *
   * The returned factory handles the full lifecycle:
   * - setup: creates the appropriate widget hierarchy (label, stack, etc.)
   * - bind: connects the row object data and establishes reactive updates (playing state, metadata)
   * - unbind: performs necessary cleanup to avoid leaks when rows are recycled
   *
   * @param field The track field to build a column factory for.
   * @param commitFn A callback to handle metadata changes if the column is editable.
   * @param playingModel Model used to drive the now-playing highlight. Each cell
   *        subscribes once to its playing-changed signal and styles from playingTrackId().
   */
  Glib::RefPtr<Gtk::SignalListItemFactory> buildColumnFactory(rt::TrackField field,
                                                              MetadataCommitFn const& commitFn,
                                                              TrackListModel& playingModel);
} // namespace ao::gtk

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackRowObject.h"
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

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
  using MetadataEditSessionFn =
    std::function<Result<uimodel::TrackAuthoringSession>(Glib::RefPtr<TrackRowObject> const& rowPtr)>;
  using MetadataCommitFn = std::function<void(Glib::RefPtr<TrackRowObject> const& rowPtr,
                                              rt::TrackField field,
                                              std::string newValue,
                                              uimodel::TrackAuthoringSession& session)>;

  /**
   * Builds a SignalListItemFactory for the given track field.
   *
   * The returned factory handles the full lifecycle:
   * - setup: creates the cell widgets and subscribes once to model playing changes
   * - bind: observes the bound row's fields and applies current field and playing state
   * - unbind: disconnects the field observer, retires any edit session, and releases the row
   *
   * @param field The track field to build a column factory for.
   * @param beginEditSession A callback to capture an authoring session for the bound row.
   * @param commitFn A callback to handle metadata changes if the column is editable.
   * @param model The model that supplies display fields and now-playing state.
   */
  Glib::RefPtr<Gtk::SignalListItemFactory> buildColumnFactory(rt::TrackField field,
                                                              MetadataEditSessionFn const& beginEditSession,
                                                              MetadataCommitFn const& commitFn,
                                                              TrackListModel& model);
} // namespace ao::gtk

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackViewPage.h"

#include <ao/library/MusicLibrary.h>
#include <ao/model/TrackIdList.h>
#include <gtkmm.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ao::app
{
  class AppSession;
}
namespace ao::gtk
{
  class TrackRowDataProvider;
}

namespace ao::gtk
{
  struct TrackSelectionContext final
  {
    ao::ListId listId;
    std::vector<ao::TrackId> selectedIds;
    ao::model::TrackIdList* membershipList = nullptr;
  };

  /**
   * TagEditController handles the track context menu and tag editing dialogs.
   */
  class TagEditController final
  {
  public:
    struct Callbacks final
    {
      std::function<void(std::string const&)> onStatusMessage;
      std::function<void()> onTagsMutated;
    };

    TagEditController(Gtk::Window& parent, ao::app::AppSession& session, Callbacks callbacks);
    ~TagEditController();

    void setDataProvider(TrackRowDataProvider* provider);

    // Add to action group for menu access
    void addActionsTo(Gio::ActionMap& actionMap);

    void showTrackContextMenu(TrackViewPage& page, TrackSelectionContext const& selection, double x, double y);

    void showTagEditor(TrackSelectionContext const& selection, Gtk::Widget& relativeTo);

  private:
    void setupActions();

    void addTagToCurrentSelection(std::string const& tag);
    void removeTagFromCurrentSelection(std::string const& tag);
    void applyTagChangeToCurrentSelection(std::vector<std::string> const& tagsToAdd,
                                          std::vector<std::string> const& tagsToRemove);

    Callbacks _callbacks;
    ao::app::AppSession& _session;
    TrackRowDataProvider* _dataProvider = nullptr;

    // The explicit selection to apply the tags to
    std::optional<TrackSelectionContext> _optActiveSelection;

    // Actions
    Glib::RefPtr<Gio::SimpleAction> _trackTagAddAction;
    Glib::RefPtr<Gio::SimpleAction> _trackTagRemoveAction;
    Glib::RefPtr<Gio::SimpleAction> _trackTagToggleAction;

    std::unique_ptr<TagPopover> _tagPopover;
  };
} // namespace ao::gtk

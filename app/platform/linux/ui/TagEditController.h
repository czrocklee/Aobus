// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#pragma once

#include "platform/linux/ui/LibrarySession.h"
#include "platform/linux/ui/TrackViewPage.h"

#include <gtkmm.h>
#include <rs/library/MusicLibrary.h>
#include <rs/model/TrackIdList.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace app::ui
{
  struct TrackSelectionContext final
  {
    rs::ListId listId;
    std::vector<rs::TrackId> selectedIds;
    rs::model::TrackIdList* membershipList = nullptr;
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

    TagEditController(Gtk::Window& parent, Callbacks callbacks);
    ~TagEditController();

    void setLibrarySession(LibrarySession* session);
    
    // Add to action group for menu access
    void addActionsTo(Gio::ActionMap& actionMap);

    void showTrackContextMenu(TrackViewPage& page,
                              TrackSelectionContext const& selection,
                              double x,
                              double y);

    void showTagEditor(TrackViewPage& page,
                       TrackSelectionContext const& selection,
                       double x,
                       double y);

  private:
    void setupActions();
    
    void addTagToCurrentSelection(std::string const& tag);
    void removeTagFromCurrentSelection(std::string const& tag);
    void applyTagChangeToCurrentSelection(std::vector<std::string> const& tagsToAdd,
                                          std::vector<std::string> const& tagsToRemove);

    Gtk::Window& _parent;
    Callbacks _callbacks;
    LibrarySession* _currentSession = nullptr;

    // The explicit selection to apply the tags to
    std::optional<TrackSelectionContext> _activeSelection;

    // Actions
    Glib::RefPtr<Gio::SimpleAction> _trackTagAddAction;
    Glib::RefPtr<Gio::SimpleAction> _trackTagRemoveAction;
    Glib::RefPtr<Gio::SimpleAction> _trackTagToggleAction;
  };

} // namespace app::ui

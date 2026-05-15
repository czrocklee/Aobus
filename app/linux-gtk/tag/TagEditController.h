// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackViewPage.h"

#include <ao/library/MusicLibrary.h>
#include <gtkmm.h>
#include <runtime/TrackSource.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ao::rt
{
  class AppSession;
}
namespace ao::gtk
{
  class TrackRowCache;
}

namespace ao::gtk
{
  struct TrackSelectionContext final
  {
    ListId listId;
    std::vector<TrackId> selectedIds;
  };

  /**
   * TagEditController handles the track context menu and tag editing dialogs.
   */
  class TagEditController final
  {
  public:
    struct Callbacks final
    {
      std::function<void()> onTagsMutated;
    };

    TagEditController(Gtk::Window& parent, rt::AppSession& session, Callbacks callbacks);
    ~TagEditController();

    // Not copyable or movable
    TagEditController(TagEditController const&) = delete;
    TagEditController& operator=(TagEditController const&) = delete;
    TagEditController(TagEditController&&) = delete;
    TagEditController& operator=(TagEditController&&) = delete;

    void setDataProvider(TrackRowCache* provider);

    // Add to action group for menu access
    void addActionsTo(Gio::ActionMap& actionMap);

    void showTrackContextMenu(TrackViewPage& page, TrackSelectionContext const& selection, double posX, double posY);

    void showTagEditor(TrackSelectionContext const& selection, Gtk::Widget& relativeTo);

  private:
    void setupActions();

    void addTagToCurrentSelection(std::string const& tag);
    void removeTagFromCurrentSelection(std::string const& tag);
    void applyTagChangeToCurrentSelection(std::vector<std::string> const& tagsToAdd,
                                          std::vector<std::string> const& tagsToRemove);

    Callbacks _callbacks;
    rt::AppSession& _session;
    TrackRowCache* _dataProvider = nullptr;

    // The explicit selection to apply the tags to
    std::optional<TrackSelectionContext> _optActiveSelection;

    // Actions
    Glib::RefPtr<Gio::SimpleAction> _trackTagAddAction;
    Glib::RefPtr<Gio::SimpleAction> _trackTagRemoveAction;
    Glib::RefPtr<Gio::SimpleAction> _trackTagToggleAction;

    std::unique_ptr<TagPopover> _tagPopover;
  };
} // namespace ao::gtk

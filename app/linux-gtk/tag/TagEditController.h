// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <giomm/actionmap.h>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <glibmm/refptr.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
}
namespace ao::gtk
{
  class TrackRowCache;
  class ThemeCoordinator;
  class TagPopover;
  class TrackViewPage;
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

    TagEditController(Gtk::Window& parent,
                      rt::AppRuntime& runtime,
                      Callbacks callbacks,
                      ThemeCoordinator& themeController);
    ~TagEditController();

    // Not copyable or movable
    TagEditController(TagEditController const&) = delete;
    TagEditController& operator=(TagEditController const&) = delete;
    TagEditController(TagEditController&&) = delete;
    TagEditController& operator=(TagEditController&&) = delete;

    void setDataProvider(TrackRowCache* provider);

    // Add to action group for menu access
    void addActionsTo(Gio::ActionMap& actionMap);

    void openTrackContextMenu(TrackViewPage& page,
                              TrackSelectionContext const& selection,
                              double xPosition,
                              double yPosition);

    void openTagEditor(TrackSelectionContext const& selection, Gtk::Widget& relativeTo);
    void presentProperties(TrackSelectionContext const& selection);

    void submitTagChanges(TrackSelectionContext const& selection,
                          std::span<std::string const> tagsToAdd,
                          std::span<std::string const> tagsToRemove);

  private:
    void createActions();
    void openTagsPopover(TrackViewPage& page, double xPosition, double yPosition);
    void presentPropertiesDialog();

    void addTagToCurrentSelection(std::string tag);
    void removeTagFromCurrentSelection(std::string tag);
    void applyTagChangeToCurrentSelection(std::span<std::string const> tagsToAdd,
                                          std::span<std::string const> tagsToRemove);

    Callbacks _callbacks;
    rt::AppRuntime& _runtime;
    TrackRowCache* _dataProvider = nullptr;
    Gtk::Window& _parent;
    ThemeCoordinator& _themeController;

    // The explicit selection to apply the tags to
    std::optional<TrackSelectionContext> _optActiveSelection;

    // Actions
    Glib::RefPtr<Gio::SimpleAction> _trackTagAddActionPtr;
    Glib::RefPtr<Gio::SimpleAction> _trackTagRemoveActionPtr;
    Glib::RefPtr<Gio::SimpleAction> _trackTagToggleActionPtr;

    std::unique_ptr<TagPopover> _tagPopoverPtr;
    std::unique_ptr<Gtk::PopoverMenu> _contextPopoverPtr;
    Glib::RefPtr<Gio::SimpleActionGroup> _contextActionGroupPtr;

    TrackViewPage* _contextPage = nullptr;
    double _contextXPosition = 0.0;
    double _contextYPosition = 0.0;
  };
} // namespace ao::gtk

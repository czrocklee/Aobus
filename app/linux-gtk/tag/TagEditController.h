// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "common/ActionMapRegistration.h"
#include <ao/CoreIds.h>
#include <ao/async/LifetimeScope.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <giomm/simpleactiongroup.h>
#include <glibmm/refptr.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

#include <cstdint>
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
  enum class TrackOrderCommand : std::uint8_t;
}

namespace ao::gtk
{
  struct TrackSelection final
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
      std::function<void()> onTagsMutated{};
      std::function<void()> onManageListsRequested{};
    };

    TagEditController(Gtk::Window& parent,
                      rt::AppRuntime& runtime,
                      i18n::MessageCatalog textCatalog,
                      Callbacks callbacks,
                      ThemeCoordinator& themeCoordinator);
    ~TagEditController();

    // Not copyable or movable
    TagEditController(TagEditController const&) = delete;
    TagEditController& operator=(TagEditController const&) = delete;
    TagEditController(TagEditController&&) = delete;
    TagEditController& operator=(TagEditController&&) = delete;

    void setDataProvider(TrackRowCache* provider);

    void openTrackContextMenu(TrackViewPage& page, TrackSelection const& selection, double xPosition, double yPosition);

    void presentProperties(TrackSelection const& selection);
    void openTagEditor(TrackSelection const& selection, Gtk::Widget& relativeTo);

    void submitTagChanges(TrackSelection const& selection,
                          std::vector<std::string> tagsToAdd,
                          std::vector<std::string> tagsToRemove);

  private:
    void buildContextActionsAndMenu(TrackViewPage& page);
    void applyListMembershipToCurrentSelection(ListId listId, bool add);
    void applyListOrderToCurrentSelection(TrackOrderCommand action);
    void openTagsPopover(TrackViewPage& page, double xPosition, double yPosition);
    void presentPropertiesDialog();
    void unparentClosedContextPopover();
    void scheduleContextPopoverRetirement();
    void finishContextPopoverRetirement();
    void retireContextPopover();
    void unparentClosedTagPopover();
    void retireTagPopover();
    void observeTagPopoverAnchor();

    void applyTagChangeToCurrentSelection(std::span<std::string const> tagsToAdd,
                                          std::span<std::string const> tagsToRemove);
    bool beginTagEditSession(std::span<TrackId const> trackIds);

    Callbacks _callbacks;
    rt::AppRuntime& _runtime;
    i18n::MessageCatalog _textCatalog;
    TrackRowCache* _dataProvider = nullptr;
    Gtk::Window& _parent;
    ThemeCoordinator& _themeCoordinator;

    // The explicit selection to apply the tags to
    std::optional<TrackSelection> _optActiveSelection;

    std::unique_ptr<TagPopover> _tagPopoverPtr;
    std::optional<uimodel::TrackAuthoringSession> _optTagEditSession;
    std::uint64_t _tagEditSessionGeneration = 0;
    std::unique_ptr<Gtk::PopoverMenu> _contextPopoverPtr;
    Glib::RefPtr<Gio::SimpleActionGroup> _contextActionGroupPtr;
    ActionMapRegistration _contextActionsRegistration;

    sigc::scoped_connection _contextPopoverClosedConnection;
    sigc::scoped_connection _contextAnchorUnmapConnection;
    sigc::scoped_connection _contextPopoverRetirementConnection;
    sigc::scoped_connection _tagPopoverClosedConnection;
    sigc::scoped_connection _tagAnchorUnmapConnection;
    sigc::scoped_connection _tagsChangedConnection;
    async::LifetimeScope _tasks;

    TrackViewPage* _contextPage = nullptr;
    double _contextXPosition = 0.0;
    double _contextYPosition = 0.0;
  };
} // namespace ao::gtk

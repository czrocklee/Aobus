// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "track/TrackColumnViewHost.h"
#include "track/TrackListModel.h"
#include "track/TrackOrderActions.h"
#include "track/TrackRowObject.h"
#include "track/TrackSelectionController.h"
#include <ao/CoreIds.h>
#include <ao/async/LifetimeScope.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/library/list/ListOrder.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>
#include <sigc++/signal.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Gtk
{
  class ColumnView;
}

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::uimodel
{
  class TrackAuthoringSession;
}

namespace ao::gtk
{
  class ResourceImageLoader;
  class TagPopover;
  class TrackOrderDragController;

  class TrackViewPage final : public Gtk::Box
  {
  public:
    using SelectionChangedSignal = sigc::signal<void()>;
    using TrackActivatedSignal = sigc::signal<void(TrackId)>;
    using ContextMenuRequestedSignal = sigc::signal<void(double, double)>;
    using TagEditRequestedSignal = sigc::signal<void(std::vector<TrackId> const&, Gtk::Widget*)>;
    using CreateSmartListRequestedSignal = sigc::signal<void(std::string)>;

    explicit TrackViewPage(ListId listId,
                           Glib::RefPtr<TrackListModel> modelPtr,
                           uimodel::TrackColumnLayouts& columnLayouts,
                           i18n::MessageCatalog textCatalog,
                           rt::AppRuntime& runtime,
                           ResourceImageLoader& thumbnailLoader,
                           rt::TrackPresentationSpec const& presentation = rt::defaultTrackPresentationSpec(),
                           rt::ViewId viewId = rt::kInvalidViewId);
    ~TrackViewPage() override;

    TrackViewPage(TrackViewPage const&) = delete;
    TrackViewPage& operator=(TrackViewPage const&) = delete;
    TrackViewPage(TrackViewPage&&) = delete;
    TrackViewPage& operator=(TrackViewPage&&) = delete;

    ListId listId() const noexcept { return _listId; }
    rt::ViewId viewId() const noexcept { return _viewId; }

    TrackSelectionController& selectionController() noexcept { return _viewHostPtr->selectionController(); }

    // Stable signals forwarded from the current view host generation
    SelectionChangedSignal& signalSelectionChanged() noexcept { return _viewHostPtr->signalSelectionChanged(); }
    TrackActivatedSignal& signalTrackActivated() noexcept { return _viewHostPtr->signalTrackActivated(); }
    ContextMenuRequestedSignal& signalContextMenuRequested() noexcept
    {
      return _viewHostPtr->signalContextMenuRequested();
    }
    TagEditRequestedSignal& signalTagEditRequested() noexcept { return _viewHostPtr->signalTagEditRequested(); }

    CreateSmartListRequestedSignal& signalCreateSmartListRequested() noexcept;
    rt::TrackListProjection const* projection() const noexcept { return _modelPtr ? _modelPtr->projection() : nullptr; }
    bool hasOrderDragHandle() const noexcept { return _orderDragControllerPtr != nullptr; }
    uimodel::ListOrderCapabilityState orderCapabilities() const;
    void applyListOrderCommand(TrackOrderCommand command);

    void openTagPopover(TagPopover& popover, double xPosition, double yPosition);
    void setStatusMessage(std::string_view message);
    void clearStatusMessage();

    void setPlayingTrackId(TrackId trackId);
    void setGroupCoverPlaceholderStyle(uimodel::CoverArtPlaceholderStyle style);
    uimodel::CoverArtPlaceholderStyle groupCoverPlaceholderStyle() const noexcept
    {
      return _groupCoverPlaceholderStyle;
    }

    void applyPresentation(rt::TrackPresentationSpec const& presentation);
    void refreshOrderCapabilities();

  protected:
    void on_map() override;

  private:
    void configureHeaderFactory();
    void buildStatusBar();
    void applyColumnViewStyles(Gtk::ColumnView& view);
    void updateSectionHeaders();
    void installOrderDragController();
    void setOrderCapabilityStatus(std::string_view message);
    void clearOrderCapabilityStatus();

    void rebuildColumnView(std::span<rt::TrackField const> visibleFields);

    void commitMetadataChange(Glib::RefPtr<TrackRowObject> const& rowPtr,
                              rt::TrackField field,
                              std::string newValue,
                              uimodel::TrackAuthoringSession& session);

    // Child widgets
    Gtk::Label _statusLabel;
    std::optional<std::string> _optOrderCapabilityStatus;
    Gtk::ScrolledWindow _scrolledWindow;

    // Models
    ListId _listId;
    rt::ViewId _viewId;
    Glib::RefPtr<TrackListModel> _modelPtr;
    uimodel::TrackColumnLayouts& _columnLayouts;
    i18n::MessageCatalog _textCatalog;
    rt::AppRuntime& _runtime;
    ResourceImageLoader& _thumbnailLoader;
    Glib::RefPtr<Gtk::MultiSelection> _selectionModelPtr;
    Glib::RefPtr<Gtk::SignalListItemFactory> _sectionHeaderFactoryPtr;
    TrackId _playingTrackId{kInvalidTrackId};
    uimodel::CoverArtPlaceholderStyle _groupCoverPlaceholderStyle{
      uimodel::defaultCoverArtPlaceholderStyle(uimodel::CoverArtPlaceholderSlot::GroupHeading)};

    // Controllers (owned)
    std::unique_ptr<TrackColumnViewHost> _viewHostPtr;
    std::unique_ptr<TrackOrderDragController> _orderDragControllerPtr;

    // Bound to the process-level GtkStyleRuntime, whose handler reaches
    // _viewHostPtr. Declared after it so reverse-order destruction retires the
    // subscription first. That is the only protection on the constructor's
    // failure path, where the destructor body never runs.
    sigc::scoped_connection _themeRefreshConnection;

    // Signals
    CreateSmartListRequestedSignal _createSmartListRequested;
    async::LifetimeScope _tasks;
  };
} // namespace ao::gtk

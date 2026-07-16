// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "track/TrackColumnViewHost.h"
#include "track/TrackListModel.h"
#include "track/TrackRowObject.h"
#include "track/TrackSelectionController.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>
#include <sigc++/signal.h>

#include <memory>
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
  class ThumbnailLoader;
  class TagPopover;

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
                           uimodel::TrackColumnLayoutStore& layoutStore,
                           rt::AppRuntime& runtime,
                           ThumbnailLoader& thumbnailLoader,
                           rt::ViewId viewId = rt::kInvalidViewId);
    ~TrackViewPage() override;

    TrackViewPage(TrackViewPage const&) = delete;
    TrackViewPage& operator=(TrackViewPage const&) = delete;
    TrackViewPage(TrackViewPage&&) = delete;
    TrackViewPage& operator=(TrackViewPage&&) = delete;

    ListId listId() const noexcept { return _listId; }

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
    rt::TrackListProjection* projection() const noexcept { return _modelPtr ? _modelPtr->projection() : nullptr; }

    void openTagPopover(TagPopover& popover, double xPosition, double yPosition);
    void setStatusMessage(std::string_view message);
    void clearStatusMessage();

    void setPlayingTrackId(TrackId trackId);

    void applyPresentation(rt::TrackPresentationSpec const& presentation);

  protected:
    void on_map() override;

  private:
    void configureHeaderFactory();
    void buildStatusBar();
    void applyColumnViewStyles(Gtk::ColumnView& view);
    void updateSectionHeaders();

    void rebuildColumnView(std::span<rt::TrackField const> visibleFields);

    void commitMetadataChange(Glib::RefPtr<TrackRowObject> const& rowPtr,
                              rt::TrackField field,
                              std::string newValue,
                              uimodel::TrackAuthoringSession& session);

    // Child widgets
    Gtk::Label _statusLabel;
    Gtk::ScrolledWindow _scrolledWindow;
    Gtk::Popover _contextPopover;

    // Models
    ListId _listId;
    rt::ViewId _viewId{};
    Glib::RefPtr<TrackListModel> _modelPtr;
    uimodel::TrackColumnLayoutStore& _layoutStore;
    rt::AppRuntime& _runtime;
    ThumbnailLoader& _thumbnailLoader;
    Glib::RefPtr<Gtk::MultiSelection> _selectionModelPtr;
    Glib::RefPtr<Gtk::SignalListItemFactory> _sectionHeaderFactoryPtr;
    TrackId _playingTrackId{kInvalidTrackId};

    sigc::scoped_connection _themeRefreshConnection;
    sigc::scoped_connection _modelChangedConnection;

    // Controllers (owned)
    std::unique_ptr<TrackColumnViewHost> _viewHostPtr;

    // Signals
    CreateSmartListRequestedSignal _createSmartListRequested;
  };
} // namespace ao::gtk

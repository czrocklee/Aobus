// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "tag/TagPopover.h"
#include "track/TrackColumnViewHost.h"
#include "track/TrackListModel.h"
#include "track/TrackPresentationStore.h"
#include "track/TrackRowObject.h"
#include "track/TrackSelectionController.h"
#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/columnview.h>
#include <gtkmm/label.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/sortlistmodel.h>
#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>
#include <sigc++/signal.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  class ImageCache;

  class TrackViewPage final : public Gtk::Box
  {
  public:
    using SelectionChangedSignal = sigc::signal<void()>;
    using TrackActivatedSignal = sigc::signal<void(TrackId)>;
    using ContextMenuRequestedSignal = sigc::signal<void(double, double)>;
    using TagEditRequestedSignal = sigc::signal<void(std::vector<TrackId> const&, Gtk::Widget*)>;
    using CreateSmartListRequestedSignal = sigc::signal<void(std::string)>;

    explicit TrackViewPage(ListId listId,
                           Glib::RefPtr<TrackListModel> model,
                           TrackPresentationStore& presentationStore,
                           rt::AppRuntime& runtime,
                           ImageCache& imageCache,
                           rt::ViewId viewId = rt::kInvalidViewId);
    ~TrackViewPage() override;

    TrackViewPage(TrackViewPage const&) = delete;
    TrackViewPage& operator=(TrackViewPage const&) = delete;
    TrackViewPage(TrackViewPage&&) = delete;
    TrackViewPage& operator=(TrackViewPage&&) = delete;

    ListId listId() const noexcept { return _listId; }

    TrackSelectionController& selectionController() noexcept { return _viewHost->selectionController(); }

    // Stable signals forwarded from the current view host generation
    SelectionChangedSignal& signalSelectionChanged() noexcept { return _viewHost->signalSelectionChanged(); }
    TrackActivatedSignal& signalTrackActivated() noexcept { return _viewHost->signalTrackActivated(); }
    ContextMenuRequestedSignal& signalContextMenuRequested() noexcept
    {
      return _viewHost->signalContextMenuRequested();
    }
    TagEditRequestedSignal& signalTagEditRequested() noexcept { return _viewHost->signalTagEditRequested(); }

    CreateSmartListRequestedSignal& signalCreateSmartListRequested() noexcept;
    rt::ITrackListProjection* projection() const noexcept { return _model ? _model->projection() : nullptr; }

    void showTagPopover(TagPopover& popover, double posX, double posY);
    void setStatusMessage(std::string_view message);
    void clearStatusMessage();

    void setPlayingTrackId(TrackId trackId);

    void applyPresentation(rt::TrackPresentationSpec const& presentation);

  protected:
    void on_map() override;

  private:
    void setupHeaderFactory();
    void setupStatusBar();
    void setupColumnViewStyles(Gtk::ColumnView& view);
    void updateSectionHeaders();

    void rebuildColumnView(std::span<rt::TrackField const> visibleFields);

    void commitMetadataChange(Glib::RefPtr<TrackRowObject> const& row, rt::TrackField field, std::string newValue);

    // Child widgets
    Gtk::Label _statusLabel;
    Gtk::ScrolledWindow _scrolledWindow;
    Gtk::Popover _contextPopover;

    // Models
    ListId _listId;
    rt::ViewId _viewId{};
    Glib::RefPtr<TrackListModel> _model;
    TrackPresentationStore& _presentationStore;
    rt::AppRuntime& _runtime;
    ImageCache& _imageCache;
    Glib::RefPtr<Gtk::SortListModel> _groupModel;
    Glib::RefPtr<Gtk::MultiSelection> _selectionModel;
    Glib::RefPtr<Gtk::SignalListItemFactory> _sectionHeaderFactory;
    TrackId _playingTrackId{kInvalidTrackId};

    sigc::scoped_connection _themeRefreshConnection;
    sigc::scoped_connection _modelChangedConnection;

    // Controllers (owned)
    std::unique_ptr<TrackColumnViewHost> _viewHost;

    // Signals
    CreateSmartListRequestedSignal _createSmartListRequested;
  };
} // namespace ao::gtk

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "tag/TagPopover.h"
#include "track/TrackColumnViewHost.h"
#include "track/TrackFilterController.h"
#include "track/TrackListAdapter.h"
#include "track/TrackPresentation.h"
#include "track/TrackPresentationStore.h"
#include <runtime/CorePrimitives.h>
#include <runtime/ProjectionTypes.h>

#include <gtkmm.h>

#include <memory>
#include <string>
#include <vector>

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk
{
  class TrackViewPage final : public Gtk::Box
  {
  public:
    using SelectionChangedSignal = sigc::signal<void()>;
    using TrackActivatedSignal = sigc::signal<void(TrackId)>;
    using ContextMenuRequestedSignal = sigc::signal<void(double, double)>;
    using TagEditRequestedSignal = sigc::signal<void(std::vector<TrackId> const&, Gtk::Widget*)>;
    using CreateSmartListRequestedSignal = sigc::signal<void(std::string)>;

    explicit TrackViewPage(ListId listId,
                           TrackListAdapter& adapter,
                           TrackColumnLayoutModel& columnLayoutModel,
                           TrackPresentationStore& presentationStore,
                           rt::AppSession& session,
                           rt::ViewId viewId = rt::ViewId{});
    ~TrackViewPage() override;

    TrackViewPage(TrackViewPage const&) = delete;
    TrackViewPage& operator=(TrackViewPage const&) = delete;
    TrackViewPage(TrackViewPage&&) = delete;
    TrackViewPage& operator=(TrackViewPage&&) = delete;

    ListId getListId() const noexcept { return _listId; }

    TrackFilterController& filterController() noexcept { return *_filterController; }
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
    rt::ITrackListProjection* projection() const noexcept { return _adapter.projection(); }

    void showTagPopover(TagPopover& popover, double posX, double posY);
    void setStatusMessage(std::string_view message);
    void clearStatusMessage();

    void setPlayingTrackId(std::optional<TrackId> optPlayingTrackId);

  private:
    void setupPresentationControls();
    void setupHeaderFactory();
    void setupStatusBar();
    void setupColumnViewStyles(Gtk::ColumnView& view);
    void updateSectionHeaders();

    void populatePresentationOptions();
    void onPresentationSelected(std::string_view presentationId);
    void onCreateCustomViewClicked();
    void applyPresentation(rt::TrackPresentationSpec const& presentation);
    void applyPresentation(rt::TrackListPresentationSnapshot const& snapshot);

    void rebuildColumnView(TrackColumnLayout const& layout);

    void commitMetadataChange(Glib::RefPtr<TrackRowObject> const& row, TrackColumn column, std::string const& newValue);

    // Child widgets
    Gtk::Box _controlsBar{Gtk::Orientation::HORIZONTAL};
    Gtk::Entry _filterEntry;
    Gtk::MenuButton _presentationButton;
    Gtk::Popover _presentationPopover;
    Gtk::Box _presentationMenuBox{Gtk::Orientation::VERTICAL};
    Gtk::Label _statusLabel;
    Gtk::ScrolledWindow _scrolledWindow;
    Gtk::Popover _contextPopover;

    // Models
    ListId _listId;
    rt::ViewId _viewId{};
    TrackListAdapter& _adapter;
    TrackPresentationStore& _presentationStore;
    rt::AppSession& _session;
    Glib::RefPtr<Gtk::SortListModel> _groupModel;
    Glib::RefPtr<Gtk::MultiSelection> _selectionModel;
    TrackColumnLayoutModel& _columnLayoutModel;
    Glib::RefPtr<Gtk::SignalListItemFactory> _sectionHeaderFactory;
    rt::TrackPresentationSpec _activePresentation;
    std::optional<TrackId> _optPlayingTrackId;

    sigc::scoped_connection _themeRefreshConnection;

    // Controllers (owned)
    std::unique_ptr<TrackColumnViewHost> _viewHost;
    std::unique_ptr<TrackFilterController> _filterController;

    // Signals
    CreateSmartListRequestedSignal _createSmartListRequested;
  };
} // namespace ao::gtk

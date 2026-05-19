// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackViewPage.h"
#include "app/StyleManager.h"
#include "tag/TagPopover.h"
#include "track/TrackColumnFactoryBuilder.h"
#include "track/TrackColumnViewHost.h"
#include "track/TrackListAdapter.h"
#include "track/TrackPresentation.h"
#include "track/TrackPresentationStore.h"
#include "track/TrackRowObject.h"
#include <ao/Type.h>
#include <ao/utility/Log.h>
#include <runtime/AppRuntime.h>
#include <runtime/CorePrimitives.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/ProjectionTypes.h>
#include <runtime/StateTypes.h>
#include <runtime/TrackPresentationPreset.h>
#include <runtime/ViewService.h>

#include <gdkmm/rectangle.h>
#include <glib/gtypes.h>
#include <glibmm/main.h>
#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <glibmm/wrap.h>
#include <gobject/gobject.h>
#include <gtk/gtkstyleprovider.h>
#include <gtkmm/box.h>
#include <gtkmm/columnview.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listheader.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/selectionmodel.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/sorter.h>
#include <gtkmm/sortlistmodel.h>

#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    std::string trackCountLabel(::guint count)
    {
      return std::format("{} {}", count, count == 1 ? "track" : "tracks");
    }

    TrackRowObject* trackRowFromSorterItem(gpointer item)
    {
      auto* const object = static_cast<GObject*>(item);
      return object != nullptr ? dynamic_cast<TrackRowObject*>(Glib::wrap_auto(object, false)) : nullptr;
    }

    class ProjectionGroupSectionSorter final : public Gtk::Sorter
    {
    public:
      static Glib::RefPtr<ProjectionGroupSectionSorter> create(TrackListAdapter& adapter)
      {
        return Glib::make_refptr_for_instance<ProjectionGroupSectionSorter>(
          new ProjectionGroupSectionSorter(adapter)); // NOLINT(cppcoreguidelines-owning-memory)
      }

    protected:
      explicit ProjectionGroupSectionSorter(TrackListAdapter& adapter)
        : Glib::ObjectBase{typeid(ProjectionGroupSectionSorter)}, Gtk::Sorter{}, _adapter{adapter}
      {
      }

      Gtk::Ordering compare_vfunc(::gpointer item1, ::gpointer item2) override
      {
        auto const* const lhs = trackRowFromSorterItem(item1);
        auto const* const rhs = trackRowFromSorterItem(item2);

        if (lhs == nullptr || rhs == nullptr)
        {
          return Gtk::Ordering::EQUAL;
        }

        auto const optLhsGroup = _adapter.groupIndexForTrack(lhs->getTrackId());
        auto const optRhsGroup = _adapter.groupIndexForTrack(rhs->getTrackId());

        if (!optLhsGroup || !optRhsGroup || *optLhsGroup == *optRhsGroup)
        {
          return Gtk::Ordering::EQUAL;
        }

        return *optLhsGroup < *optRhsGroup ? Gtk::Ordering::SMALLER : Gtk::Ordering::LARGER;
      }

      Order get_order_vfunc() override { return Order::PARTIAL; }

    private:
      TrackListAdapter& _adapter;
    };
  } // namespace

  TrackViewPage::TrackViewPage(ListId listId,
                               TrackListAdapter& adapter,
                               TrackColumnLayoutModel& columnLayoutModel,
                               TrackPresentationStore& presentationStore,
                               rt::AppRuntime& runtime,
                               rt::ViewId viewId)
    : Gtk::Box{Gtk::Orientation::VERTICAL}
    , _listId{listId}
    , _viewId{viewId}
    , _adapter{adapter}
    , _presentationStore{presentationStore}
    , _runtime{runtime}
    , _groupModel{Gtk::SortListModel::create(adapter.getModel(), Glib::RefPtr<Gtk::Sorter>{})}
    , _selectionModel{Gtk::MultiSelection::create(_groupModel)}
    , _columnLayoutModel{columnLayoutModel}
    , _viewHost{std::make_unique<TrackColumnViewHost>(_adapter, _columnLayoutModel, _selectionModel)}
  {
    _viewHost->setupSelectionActivation();

    _modelChangedConnection = _adapter.signalModelChanged().connect(
      [this]
      {
        APP_LOG_INFO("TrackViewPage: Underlying model changed, updating SortListModel...");
        _groupModel->set_model(_adapter.getModel());
      });

    _themeRefreshConnection = StyleManager::instance().signalRefreshed().connect(
      [this]
      {
        APP_LOG_INFO("Executing theme refresh for TrackViewPage...");
        _viewHost->columnController().updateTitlePositionVariable();
        _viewHost->columnView().queue_draw();
      });

    setupStatusBar();
    setupHeaderFactory();

    // 1. Configure columns and layout first (Off-tree)
    _viewHost->setupColumns(
      [this](TrackColumnDefinition const& def)
      { return buildColumnFactory(def, std::bind_front(&TrackViewPage::commitMetadataChange, this)); });

    if (_viewId != rt::ViewId{})
    {
      auto const& presState = _runtime.views().trackListState(_viewId).presentation;
      _viewHost->columnController().setLayoutAndApply(
        trackColumnLayoutForPresentation(rt::presentationSpecFromState(presState)));
    }
    else
    {
      _viewHost->columnController().syncLayout();
    }

    // 2. Configure decorators and styles
    updateSectionHeaders();

    setupColumnViewStyles(_viewHost->columnView());

    _contextPopover.set_has_arrow(false);

    // 3. Finally attach model and add to scrolled window
    _viewHost->columnView().set_model(_selectionModel);

    _scrolledWindow.set_child(_viewHost->columnView());
    _scrolledWindow.set_vexpand(true);
    _scrolledWindow.set_hexpand(true);
    _contextPopover.set_parent(_viewHost->columnView());

    append(_statusLabel);
    append(_scrolledWindow);
  }

  TrackViewPage::~TrackViewPage() = default;

  void TrackViewPage::setupHeaderFactory()
  {
    _sectionHeaderFactory = Gtk::SignalListItemFactory::create();

    _sectionHeaderFactory->signal_setup_obj().connect(
      [](Glib::RefPtr<Glib::Object> const& object)
      {
        auto const header = std::dynamic_pointer_cast<Gtk::ListHeader>(object);

        if (!header)
        {
          return;
        }

        auto* const label = Gtk::make_managed<Gtk::Label>("");

        label->set_halign(Gtk::Align::START);
        label->add_css_class("ao-track-section-header");
        label->set_xalign(0.0F);

        header->set_child(*label);
      });

    _sectionHeaderFactory->signal_bind_obj().connect(
      [this](Glib::RefPtr<Glib::Object> const& object)
      {
        auto const header = std::dynamic_pointer_cast<Gtk::ListHeader>(object);
        auto* const label = header ? dynamic_cast<Gtk::Label*>(header->get_child()) : nullptr;

        if (header == nullptr || label == nullptr)
        {
          return;
        }

        auto text = std::string{};

        if (auto* const proj = _adapter.projection())
        {
          auto const start = header->get_start();

          if (auto const optGroupIndex = proj->groupIndexAt(start); optGroupIndex)
          {
            text = proj->groupAt(*optGroupIndex).label;
          }
        }

        if (!text.empty())
        {
          text += " ";
        }

        text += "(" + trackCountLabel(header->get_n_items()) + ")";

        label->set_text(text);
      });

    _groupModel->set_section_sorter(ProjectionGroupSectionSorter::create(_adapter));
    _viewHost->columnView().set_header_factory(_sectionHeaderFactory);
  }

  void TrackViewPage::setupStatusBar()
  {
    _statusLabel.set_visible(false);
    _statusLabel.set_halign(Gtk::Align::START);
    _statusLabel.set_valign(Gtk::Align::CENTER);
    _statusLabel.add_css_class("ao-track-status-message");

    auto const context = _statusLabel.get_style_context();
    context->add_class("dim-label");
  }

  void TrackViewPage::setStatusMessage(std::string_view message)
  {
    _statusLabel.set_text(std::string{message});

    if (message.empty())
    {
      clearStatusMessage();
      return;
    }

    _statusLabel.set_visible(true);
  }

  void TrackViewPage::clearStatusMessage()
  {
    _statusLabel.set_text("");
    _statusLabel.set_visible(false);
  }

  void TrackViewPage::applyPresentation(rt::TrackPresentationSpec const& presentation)
  {
    rebuildColumnView(trackColumnLayoutForPresentation(presentation));
  }

  void TrackViewPage::applyPresentation(rt::TrackListPresentationSnapshot const& snapshot)
  {
    rebuildColumnView(trackColumnLayoutForPresentation(snapshot));
  }

  void TrackViewPage::setPlayingTrackId(std::optional<TrackId> optPlayingTrackId)
  {
    _optPlayingTrackId = optPlayingTrackId;
    _viewHost->selectionController().setPlayingTrackId(optPlayingTrackId);
  }

  void TrackViewPage::rebuildColumnView(TrackColumnLayout const& layout)
  {
    auto const factoryProvider = [this](TrackColumnDefinition const& def)
    { return buildColumnFactory(def, std::bind_front(&TrackViewPage::commitMetadataChange, this)); };

    // 1. Detach UI from Model and Tree immediately.
    _viewHost->columnView().set_model(Glib::RefPtr<Gtk::SelectionModel>{});
    _scrolledWindow.unset_child();
    _contextPopover.unparent();

    // 2. Create a new generation off-tree.
    auto& newView = _viewHost->rebuild(_adapter, _columnLayoutModel, _selectionModel, factoryProvider);

    // 3. Configure structural properties before attaching model (Safe)
    setupColumnViewStyles(newView);

    _viewHost->columnController().setLayoutAndApply(layout);
    _viewHost->columnController().updateTitlePositionVariable();

    // 4. Apply decorations (Section Headers)
    updateSectionHeaders();

    // 5. Restore playing state in the new controller before attaching model
    if (_optPlayingTrackId)
    {
      _viewHost->selectionController().setPlayingTrackId(_optPlayingTrackId);
    }

    // 6. Attach the model
    newView.set_model(_selectionModel);

    // 7. Swap the child in the live UI tree
    _scrolledWindow.set_child(newView);
    _contextPopover.set_parent(newView);

    // 8. Restore scroll position to selection if possible (Deferred to idle for stability)
    Glib::signal_idle().connect_once(
      [this]
      {
        if (auto const optPrimaryId = _viewHost->selectionController().getPrimarySelectedTrackId())
        {
          _viewHost->selectionController().scrollToTrack(*optPrimaryId);
        }
      });

    _viewHost->setupSelectionActivation();
  }

  void TrackViewPage::updateSectionHeaders()
  {
    auto* const proj = _adapter.projection();
    auto const groupBy = proj != nullptr ? proj->presentation().groupBy : rt::TrackGroupKey::None;

    if (groupBy == rt::TrackGroupKey::None)
    {
      _groupModel->set_section_sorter({});
      _viewHost->columnView().set_header_factory({});
      return;
    }

    _groupModel->set_section_sorter(ProjectionGroupSectionSorter::create(_adapter));
    _viewHost->columnView().set_header_factory(_sectionHeaderFactory);
  }

  TrackViewPage::CreateSmartListRequestedSignal& TrackViewPage::signalCreateSmartListRequested() noexcept
  {
    return _createSmartListRequested;
  }

  void TrackViewPage::showTagPopover(TagPopover& popover, double posX, double posY)
  {
    auto const rect = Gdk::Rectangle{static_cast<int>(posX), static_cast<int>(posY), 1, 1};

    if (popover.get_parent() != &_viewHost->columnView())
    {
      popover.set_parent(_viewHost->columnView());
    }

    popover.set_pointing_to(rect);
    popover.popup();
  }

  void TrackViewPage::commitMetadataChange(Glib::RefPtr<TrackRowObject> const& row,
                                           TrackColumn column,
                                           std::string const& newValue)
  {
    auto const oldValue = row->getColumnText(column).raw();

    if (newValue == oldValue)
    {
      return;
    }

    auto patch = rt::MetadataPatch{};

    if (column == TrackColumn::Title)
    {
      patch.optTitle = newValue;
      row->setTitle(newValue);
    }
    else if (column == TrackColumn::Artist)
    {
      patch.optArtist = newValue;
      row->setArtist(newValue);
    }
    else if (column == TrackColumn::Album)
    {
      patch.optAlbum = newValue;
      row->setAlbum(newValue);
    }

    auto const result = _runtime.mutation().updateMetadata({row->getTrackId()}, patch);

    if (!result)
    {
      APP_LOG_ERROR("Metadata update failed: {}", result.error().message);

      if (column == TrackColumn::Title)
      {
        row->setTitle(oldValue);
      }
      else if (column == TrackColumn::Artist)
      {
        row->setArtist(oldValue);
      }
      else if (column == TrackColumn::Album)
      {
        row->setAlbum(oldValue);
      }
    }
  }

  void TrackViewPage::setupColumnViewStyles(Gtk::ColumnView& view)
  {
    view.set_show_row_separators(true);
    view.set_reorderable(true);
    view.get_style_context()->add_provider(_viewHost->cssProvider(), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
} // namespace ao::gtk

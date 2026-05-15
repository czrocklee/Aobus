// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackViewPage.h"
#include "app/ThemeBus.h"
#include "layout/LayoutConstants.h"
#include "tag/TagPopover.h"
#include "track/TrackColumnFactoryBuilder.h"
#include "track/TrackColumnViewHost.h"
#include "track/TrackCustomViewDialog.h"
#include "track/TrackFilterController.h"
#include "track/TrackListAdapter.h"
#include "track/TrackPresentation.h"
#include "track/TrackPresentationStore.h"
#include "track/TrackRowObject.h"
#include <ao/Type.h>
#include <ao/utility/Log.h>
#include <runtime/AppSession.h>
#include <runtime/CorePrimitives.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>
#include <runtime/TrackPresentationPreset.h>
#include <runtime/ViewService.h>
#include <runtime/WorkspaceService.h>

#include <gdkmm/rectangle.h>
#include <glib-object.h>
#include <glibmm/main.h>
#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <glibmm/wrap.h>
#include <gtk/gtk.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/columnview.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listheader.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/selectionmodel.h>
#include <gtkmm/separator.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/sortlistmodel.h>
#include <gtkmm/sorter.h>
#include <gtkmm/window.h>
#include <glib.h>

#include <algorithm>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    using RowCompareFn = std::move_only_function<int(TrackRowObject const&, TrackRowObject const&)>;

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
                               rt::AppSession& session,
                               rt::ViewId viewId)
    : Gtk::Box{Gtk::Orientation::VERTICAL}
    , _listId{listId}
    , _viewId{viewId}
    , _adapter{adapter}
    , _presentationStore{presentationStore}
    , _session{session}
    , _groupModel{Gtk::SortListModel::create(adapter.getModel(), Glib::RefPtr<Gtk::Sorter>{})}
    , _columnLayoutModel{columnLayoutModel}
  {
    if (_viewId != rt::ViewId{})
    {
      auto const& presState = _session.views().trackListState(_viewId).presentation;
      _activePresentation = rt::presentationSpecFromState(presState);
    }

    _selectionModel = Gtk::MultiSelection::create(_groupModel);

    _filterController = std::make_unique<TrackFilterController>(_session.views(), _viewId, _filterEntry);
    _filterController->setStatusMessageCallback(std::bind_front(&TrackViewPage::setStatusMessage, this));
    _filterController->setCreateSmartListSignal(&_createSmartListRequested);

    _viewHost = std::make_unique<TrackColumnViewHost>(_adapter, _columnLayoutModel, _selectionModel);
    _viewHost->setupSelectionActivation();

    _themeRefreshConnection = signalThemeRefresh().connect(
      [this]
      {
        APP_LOG_INFO("Executing theme refresh for TrackViewPage...");
        _viewHost->columnController().updateTitlePositionVariable();
        _viewHost->columnView().queue_draw();
      });

    setupPresentationControls();
    setupStatusBar();
    setupHeaderFactory();

    // 1. Configure columns and layout first (Off-tree)
    _viewHost->setupColumns(
      [this](TrackColumnDefinition const& def)
      { return buildColumnFactory(def, std::bind_front(&TrackViewPage::commitMetadataChange, this)); });

    _viewHost->columnController().syncLayout();

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

    append(_controlsBar);
    append(_statusLabel);
    append(_scrolledWindow);
  }

  TrackViewPage::~TrackViewPage() = default;

  void TrackViewPage::setupPresentationControls()
  {
    _controlsBar.set_spacing(layout::kSpacingLarge);
    _controlsBar.set_margin_start(4);
    _controlsBar.set_margin_end(4);
    _controlsBar.set_margin_top(4);
    _controlsBar.set_margin_bottom(4);

    _filterEntry.set_placeholder_text("Quick filter or expression...");
    _filterEntry.set_hexpand(true);
    _filterEntry.set_icon_from_icon_name("system-search-symbolic", Gtk::Entry::IconPosition::PRIMARY);
    _filterEntry.set_icon_sensitive(Gtk::Entry::IconPosition::PRIMARY, false);
    _filterEntry.set_icon_from_icon_name("list-add-symbolic", Gtk::Entry::IconPosition::SECONDARY);
    _filterEntry.set_icon_activatable(true, Gtk::Entry::IconPosition::SECONDARY);
    _filterEntry.set_icon_tooltip_text("Create smart list from current filter", Gtk::Entry::IconPosition::SECONDARY);
    _filterEntry.set_menu_entry_icon_text("Create smart list from current filter", Gtk::Entry::IconPosition::SECONDARY);

    auto const* initialPreset = rt::builtinTrackPresentationPreset(_activePresentation.id);

    _presentationButton.set_label(initialPreset != nullptr ? std::string{initialPreset->label}
                                                           : std::string{_activePresentation.id});
    _presentationButton.set_has_frame(true);

    _presentationPopover.set_has_arrow(false);
    _presentationPopover.set_child(_presentationMenuBox);

    _presentationButton.set_popover(_presentationPopover);

    populatePresentationOptions();

    _controlsBar.append(_filterEntry);
    _controlsBar.append(_presentationButton);
  }

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
        label->set_margin_start(layout::kSpacingLarge);
        label->set_margin_end(layout::kSpacingLarge);
        label->set_margin_top(layout::kSpacingLarge);
        label->set_margin_bottom(layout::kMarginSmall);
        label->set_xalign(0.0F);

        header->set_child(*label);
      });

    _sectionHeaderFactory->signal_bind_obj().connect(
      [this](Glib::RefPtr<Glib::Object> const& object)
      {
        auto const header = std::dynamic_pointer_cast<Gtk::ListHeader>(object);
        auto* const label = header ? dynamic_cast<Gtk::Label*>(header->get_child()) : nullptr;

        if (!header || !label)
        {
          return;
        }

        auto text = std::string{};

        if (auto* proj = _adapter.projection())
        {
          auto const start = header->get_start();

          if (auto optGroupIndex = proj->groupIndexAt(start); optGroupIndex)
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

  void TrackViewPage::setupStatusBar()
  {
    _statusLabel.set_visible(false);
    _statusLabel.set_halign(Gtk::Align::START);
    _statusLabel.set_valign(Gtk::Align::CENTER);
    _statusLabel.set_margin_start(4);
    _statusLabel.set_margin_end(4);
    _statusLabel.set_margin_top(2);
    _statusLabel.set_margin_bottom(2);

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

  void TrackViewPage::populatePresentationOptions()
  {
    auto* child = _presentationMenuBox.get_first_child();

    while (child != nullptr)
    {
      auto* const next = child->get_next_sibling();
      _presentationMenuBox.remove(*child);
      child = next;
    }

    auto const builtins = _presentationStore.builtinPresets();

    for (auto const& preset : builtins)
    {
      auto* const btn = Gtk::make_managed<Gtk::Button>(std::string{preset.label});
      btn->set_halign(Gtk::Align::FILL);
      btn->set_has_frame(false);
      btn->get_style_context()->add_class("flat");

      auto const id = preset.spec.id;
      btn->signal_clicked().connect([this, id] { onPresentationSelected(id); });

      _presentationMenuBox.append(*btn);
    }

    auto const& customs = _presentationStore.customPresentations();

    if (!customs.empty())
    {
      auto* const sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
      sep->set_margin_top(4);
      sep->set_margin_bottom(4);
      _presentationMenuBox.append(*sep);

      for (auto const& custom : customs)
      {
        auto* const btn = Gtk::make_managed<Gtk::Button>(custom.label);
        btn->set_halign(Gtk::Align::FILL);
        btn->set_has_frame(false);
        btn->get_style_context()->add_class("flat");

        auto const id = custom.id;
        btn->signal_clicked().connect([this, id] { onPresentationSelected(id); });

        _presentationMenuBox.append(*btn);
      }
    }

    auto* const finalSep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    finalSep->set_margin_top(4);
    finalSep->set_margin_bottom(4);
    _presentationMenuBox.append(*finalSep);

    auto* const createBtn = Gtk::make_managed<Gtk::Button>("Create Custom View...");
    createBtn->set_halign(Gtk::Align::FILL);
    createBtn->set_has_frame(false);
    createBtn->get_style_context()->add_class("flat");
    createBtn->signal_clicked().connect([this] { onCreateCustomViewClicked(); });
    _presentationMenuBox.append(*createBtn);
  }

  void TrackViewPage::onCreateCustomViewClicked()
  {
    _presentationPopover.popdown();

    auto* parentWindow = dynamic_cast<Gtk::Window*>(get_root());

    if (parentWindow == nullptr)
    {
      return;
    }

    auto const label = std::string{_presentationButton.get_label()} + " Copy";
    auto dialog = TrackCustomViewDialog{*parentWindow, _activePresentation, label};

    if (auto const optResult = dialog.runDialog())
    {
      if (optResult->deleted)
      {
        _presentationStore.removeCustomPresentation(optResult->state.id);

        if (_activePresentation.id == optResult->state.id)
        {
          onPresentationSelected(rt::kDefaultTrackPresentationId);
        }
      }
      else
      {
        _presentationStore.addCustomPresentation(optResult->state);
        onPresentationSelected(optResult->state.id);
      }

      populatePresentationOptions();
    }
  }

  void TrackViewPage::onPresentationSelected(std::string_view presentationId)
  {
    _presentationPopover.popdown();

    if (_viewId == rt::ViewId{})
    {
      return;
    }

    auto const optSpec = _presentationStore.specForId(presentationId);

    if (!optSpec)
    {
      return;
    }

    std::string label = std::string{presentationId};

    if (auto const* builtin = rt::builtinTrackPresentationPreset(presentationId))
    {
      label = std::string{builtin->label};
    }
    else
    {
      auto const& customs = _presentationStore.customPresentations();
      auto const it = std::ranges::find(customs, presentationId, &CustomTrackPresentationState::id);

      if (it != customs.end())
      {
        label = it->label;
      }
    }

    _presentationButton.set_label(label);

    auto spec = rt::TrackPresentationSpec{*optSpec};
    Glib::signal_idle().connect_once(
      [this, spec = std::move(spec)]
      {
        _session.views().setPresentation(_viewId, spec);
        _activePresentation = spec;

        rebuildColumnView(trackColumnLayoutForPresentation(spec));
      });
  }

  void TrackViewPage::applyPresentation(rt::TrackPresentationSpec const& presentation)
  {
    _activePresentation = presentation;
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
    // This ensures the old View is "floating" and not receiving signals
    // before its C++ wrapper is destroyed by the host.
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
    // Must be called after rebuild() so _viewHost->columnView() targets the new generation.
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

    auto const result = _session.mutation().updateMetadata({row->getTrackId()}, patch);

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

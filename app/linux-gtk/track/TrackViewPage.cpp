// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackViewPage.h"

#include "layout/LayoutConstants.h"
#include "shell/ThemeBus.h"
#include "track/TrackColumnFactoryBuilder.h"
#include "track/TrackFilterController.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include "track/TrackSelectionController.h"
#include <ao/utility/Log.h>
#include <runtime/AppSession.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>
#include <runtime/ViewService.h>
#include <runtime/WorkspaceService.h>

#include <gdk/gdk.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/cursor.h>
#include <glibmm/bytes.h>
#include <glibmm/value.h>
#include <glibmm/wrap.h>
#include <gtk/gtk.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/label.h>
#include <gtkmm/listheader.h>
#include <gtkmm/listitem.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/stylecontext.h>

#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <unordered_set>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    using RowCompareFn = std::move_only_function<int(TrackRowObject const&, TrackRowObject const&)>;

    std::uint32_t dropdownPositionFor(ao::rt::TrackGroupKey groupBy)
    {
      return static_cast<std::uint32_t>(groupBy);
    }

    ao::rt::TrackGroupKey groupByFromDropdownPosition(std::uint32_t position)
    {
      if (position <= static_cast<std::uint32_t>(ao::rt::TrackGroupKey::Year))
      {
        return static_cast<ao::rt::TrackGroupKey>(position);
      }

      return ao::rt::TrackGroupKey::None;
    }

    std::string trackCountLabel(::guint count)
    {
      auto label = std::format("{}", count);

      label += count == 1 ? " track" : " tracks";

      return label;
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

        auto const lhsGroup = _adapter.groupIndexForTrack(lhs->getTrackId());
        auto const rhsGroup = _adapter.groupIndexForTrack(rhs->getTrackId());

        if (!lhsGroup || !rhsGroup || *lhsGroup == *rhsGroup)
        {
          return Gtk::Ordering::EQUAL;
        }

        return *lhsGroup < *rhsGroup ? Gtk::Ordering::SMALLER : Gtk::Ordering::LARGER;
      }

      Order get_order_vfunc() override { return Order::PARTIAL; }

    private:
      TrackListAdapter& _adapter;
    };
  }

  TrackViewPage::TrackViewPage(ao::ListId listId,
                               TrackListAdapter& adapter,
                               TrackColumnLayoutModel& columnLayoutModel,
                               ao::rt::AppSession& session,
                               ao::rt::ViewId viewId)
    : Gtk::Box{Gtk::Orientation::VERTICAL}
    , _listId{listId}
    , _viewId{viewId}
    , _adapter{adapter}
    , _session{session}
    , _groupModel{Gtk::SortListModel::create(adapter.getModel(), Glib::RefPtr<Gtk::Sorter>{})}
    , _columnLayoutModel{columnLayoutModel}
  {
    if (_viewId != ao::rt::ViewId{})
    {
      _activeGroupBy = _session.views().trackListState(_viewId).groupBy;
    }

    _selectionModel = Gtk::MultiSelection::create(_groupModel);

    _filterController = std::make_unique<TrackFilterController>(_session, _viewId, _filterEntry);
    _filterController->setStatusMessageCallback([this](std::string_view msg) { setStatusMessage(msg); });
    _filterController->setCreateSmartListSignal(&_createSmartListRequested);

    _selectionController = std::make_unique<TrackSelectionController>(_columnView, _adapter, _selectionModel);
    _selectionController->setupActivation();

    _columnController = std::make_unique<TrackColumnController>(_columnView, _columnLayoutModel);
    _columnController->setRedundancyProvider(
      [this]
      {
        auto redundant = std::unordered_set<TrackColumn>{};

        if (auto* const proj = _adapter.projection(); proj != nullptr)
        {
          for (auto const& field : proj->presentation().redundantFields)
          {
            if (auto const col = redundantFieldToColumn(field))
            {
              redundant.insert(*col);
            }
          }
        }
        return redundant;
      });

    signalThemeRefresh().connect(
      [this]
      {
        APP_LOG_INFO("Executing theme refresh for TrackViewPage...");
        _columnController->updateTitlePositionVariable();
        _columnView.queue_draw();
      });

    _columnView.get_style_context()->add_provider(
      _columnController->cssProvider(), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    setupPresentationControls();
    setupStatusBar();
    setupHeaderFactory();

    _columnView.set_model(_selectionModel);
    updateSectionHeaders();
    _contextPopover.set_has_arrow(false);
    _contextPopover.set_parent(_columnView);

    _columnView.set_show_row_separators(true);
    _columnView.set_reorderable(true);

    auto const commitFn = [this](
                            Glib::RefPtr<TrackRowObject> const& row, TrackColumn column, std::string const& newValue)
    { commitMetadataChange(row, column, newValue); };

    _columnController->setupColumns([&commitFn](TrackColumnDefinition const& def)
                                    { return buildColumnFactory(def, commitFn); });
    _columnController->setupColumnControls();

    _scrolledWindow.set_child(_columnView);
    _scrolledWindow.set_vexpand(true);
    _scrolledWindow.set_hexpand(true);

    _columnController->updateColumnVisibility();
    _columnController->applyColumnLayout();

    append(_controlsBar);
    append(_statusLabel);
    append(_scrolledWindow);

    _adapter.signalModelChanged().connect(sigc::mem_fun(*this, &TrackViewPage::onModelChanged));
    _columnController->updateTitlePositionVariable();
  }

  TrackViewPage::~TrackViewPage() = default;

  void TrackViewPage::setupPresentationControls()
  {
    _controlsBar.set_spacing(Layout::kSpacingLarge);
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

    _groupByLabel.set_text("Group");
    _groupByLabel.set_halign(Gtk::Align::START);
    _groupByLabel.set_valign(Gtk::Align::CENTER);

    _groupByOptions =
      Gtk::StringList::create({"None", "Artist", "Album", "Album Artist", "Genre", "Composer", "Work", "Year"});
    _groupByDropdown.set_model(_groupByOptions);
    _groupByDropdown.set_selected(dropdownPositionFor(_activeGroupBy));
    _groupByDropdown.property_selected().signal_changed().connect(
      sigc::mem_fun(*this, &TrackViewPage::onGroupByChanged));

    _controlsBar.append(_filterEntry);
    _controlsBar.append(_groupByLabel);
    _controlsBar.append(_groupByDropdown);
    _controlsBar.append(_columnController->columnsButton());
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
        label->set_margin_start(Layout::kSpacingLarge);
        label->set_margin_end(Layout::kSpacingLarge);
        label->set_margin_top(Layout::kSpacingLarge);
        label->set_margin_bottom(Layout::kMarginSmall);
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
    auto const groupBy = proj != nullptr ? proj->presentation().groupBy : ao::rt::TrackGroupKey::None;

    if (groupBy == ao::rt::TrackGroupKey::None)
    {
      _groupModel->set_section_sorter({});
      _columnView.set_header_factory({});
      return;
    }

    _groupModel->set_section_sorter(ProjectionGroupSectionSorter::create(_adapter));
    _columnView.set_header_factory(_sectionHeaderFactory);
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

  void TrackViewPage::onModelChanged()
  {
    _groupModel->set_model(_adapter.getModel());

    updateSectionHeaders();
    _columnController->updateColumnVisibility();
  }

  void TrackViewPage::onGroupByChanged()
  {
    _activeGroupBy = groupByFromDropdownPosition(_groupByDropdown.get_selected());

    if (_viewId != ao::rt::ViewId{})
    {
      _session.views().setGrouping(_viewId, _activeGroupBy);
    }

    updateSectionHeaders();
    _columnController->updateColumnVisibility();
  }

  sigc::signal<void(std::string)>& TrackViewPage::signalCreateSmartListRequested()
  {
    return _createSmartListRequested;
  }

  void TrackViewPage::showTagPopover(TagPopover& popover, double xPos, double yPos)
  {
    auto const rect = Gdk::Rectangle{static_cast<int>(xPos), static_cast<int>(yPos), 1, 1};

    popover.set_parent(_columnView);
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

    auto patch = ao::rt::MetadataPatch{};

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
} // namespace ao::gtk

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/TrackViewPage.h"
#include "platform/linux/ui/TrackRowDataProvider.h"

#include <glibmm/wrap.h>
#include <gtk/gtk.h>

#include <gdk/gdk.h>

#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/label.h>
#include <gtkmm/listheader.h>
#include <gtkmm/listitem.h>
#include <gtkmm/signallistitemfactory.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace app::ui
{

  namespace
  {
    using RowCompareFn = std::move_only_function<int(TrackRow const&, TrackRow const&)>;
    constexpr auto kTagsCellWidgetName = "track-tags-cell";

    Glib::RefPtr<Gtk::SignalListItemFactory> createTextColumnFactory(TrackColumnDefinition const& definition)
    {
      auto factory = Gtk::SignalListItemFactory::create();

      factory->signal_setup().connect(
        [definition](Glib::RefPtr<Gtk::ListItem> const& listItem)
        {
          if (definition.tagsCell)
          {
            auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
            box->set_halign(Gtk::Align::FILL);
            box->set_hexpand(true);
            box->add_css_class(kTagsCellWidgetName);

            auto* label = Gtk::make_managed<Gtk::Label>("");
            label->set_halign(Gtk::Align::START);
            label->set_ellipsize(Pango::EllipsizeMode::END);
            label->set_hexpand(true);

            box->append(*label);
            listItem->set_child(*box);
            return;
          }

          auto* label = Gtk::make_managed<Gtk::Label>("");
          label->set_halign(definition.numeric ? Gtk::Align::END : Gtk::Align::START);
          label->set_xalign(definition.numeric ? 1.0F : 0.0F);

          if (!definition.numeric)
          {
            label->set_ellipsize(Pango::EllipsizeMode::END);
          }

          listItem->set_child(*label);
        });

      factory->signal_bind().connect(
        [definition](Glib::RefPtr<Gtk::ListItem> const& listItem)
        {
          auto item = listItem->get_item();
          auto row = std::dynamic_pointer_cast<TrackRow>(item);

          if (!row)
          {
            return;
          }

          if (definition.tagsCell)
          {
            auto* box = dynamic_cast<Gtk::Box*>(listItem->get_child());
            auto* label = box ? dynamic_cast<Gtk::Label*>(box->get_first_child()) : nullptr;

            if (label)
            {
              label->set_text(row->getColumnText(definition.column));
            }

            return;
          }

          auto* label = dynamic_cast<Gtk::Label*>(listItem->get_child());

          if (label)
          {
            label->set_text(row->getColumnText(definition.column));
          }
        });

      return factory;
    }

    TrackRow const* trackRowFromItem(gconstpointer item)
    {
      if (!item)
      {
        return nullptr;
      }

      auto* object = Glib::wrap_auto(reinterpret_cast<GObject*>(const_cast<void*>(item)), false);
      return dynamic_cast<TrackRow const*>(object);
    }

    Glib::RefPtr<Gtk::Sorter> createRowSorter(RowCompareFn compare)
    {
      auto* comparePtr = new RowCompareFn{std::move(compare)};
      auto* customSorter = gtk_custom_sorter_new(
        [](gconstpointer lhs, gconstpointer rhs, gpointer userData) -> int
        {
          auto* compareFn = static_cast<RowCompareFn*>(userData);
          auto const* leftRow = trackRowFromItem(lhs);
          auto const* rightRow = trackRowFromItem(rhs);

          if (!compareFn || !leftRow || !rightRow)
          {
            return 0;
          }

          return (*compareFn)(*leftRow, *rightRow);
        },
        comparePtr,
        [](gpointer userData) { delete static_cast<RowCompareFn*>(userData); });

      return Glib::wrap(GTK_SORTER(customSorter), false);
    }

    bool isTagsCellWidget(Gtk::Widget const* widget)
    {
      for (auto current = widget; current != nullptr; current = current->get_parent())
      {
        if (current->has_css_class(kTagsCellWidgetName))
        {
          return true;
        }
      }

      return false;
    }

    std::uint32_t dropdownPositionFor(TrackGroupBy groupBy)
    {
      switch (groupBy)
      {
        case TrackGroupBy::None: return 0;
        case TrackGroupBy::Artist: return 1;
        case TrackGroupBy::Album: return 2;
        case TrackGroupBy::AlbumArtist: return 3;
        case TrackGroupBy::Genre: return 4;
        case TrackGroupBy::Composer: return 5;
        case TrackGroupBy::Work: return 6;
        case TrackGroupBy::Year: return 7;
      }

      return 0;
    }

    TrackGroupBy groupByFromDropdownPosition(std::uint32_t position)
    {
      switch (position)
      {
        case 1: return TrackGroupBy::Artist;
        case 2: return TrackGroupBy::Album;
        case 3: return TrackGroupBy::AlbumArtist;
        case 4: return TrackGroupBy::Genre;
        case 5: return TrackGroupBy::Composer;
        case 6: return TrackGroupBy::Work;
        case 7: return TrackGroupBy::Year;
        default: return TrackGroupBy::None;
      }
    }

    std::string trackCountLabel(guint count)
    {
      auto label = std::to_string(count);
      label += count == 1 ? " track" : " tracks";
      return label;
    }
  }

  TrackViewPage::TrackViewPage(rs::core::ListId listId,
                               Glib::RefPtr<TrackListAdapter> const& adapter,
                               std::shared_ptr<TrackColumnLayoutModel> columnLayoutModel)
    : Gtk::Box{Gtk::Orientation::VERTICAL}
    , _listId{listId}
    , _adapter{adapter}
    , _sortModel{Gtk::SortListModel::create(adapter->getModel(), Glib::RefPtr<Gtk::Sorter>{})}
    , _columnLayoutModel{columnLayoutModel ? std::move(columnLayoutModel) : std::make_shared<TrackColumnLayoutModel>()}
    , _presentationSpec{presentationSpecForGroup(TrackGroupBy::None)}
  {
    // Create multi-selection model to allow bulk operations
    _selectionModel = Gtk::MultiSelection::create(_sortModel);

    setupPresentationControls();

    setupHeaderFactory();

    // Set up column view
    _columnView.set_model(_selectionModel);
    _contextPopover.set_has_arrow(false);
    _contextPopover.set_parent(_columnView);

    // Show row separators (horizontal lines between rows)
    _columnView.set_show_row_separators(true);
    _columnView.set_reorderable(true);

    // Connect selection signal - takes (position, nItems) parameters
    _selectionModel->signal_selection_changed().connect(sigc::mem_fun(*this, &TrackViewPage::onSelectionChanged));

    // Set up columns
    setupColumns();
    _columnModel = _columnView.get_columns();

    if (_columnModel)
    {
      _columnModelChangedConnection = _columnModel->signal_items_changed().connect(
        [this](guint, guint, guint)
        {
          if (_syncingColumnLayout)
          {
            return;
          }

          queueSharedColumnLayoutUpdate();
        });
    }

    _columnLayoutChangedConnection =
      _columnLayoutModel->signalChanged().connect(sigc::mem_fun(*this, &TrackViewPage::applyColumnLayout));

    // Set up activation (double-click, Enter key)
    setupActivation();

    // Set up scrolled window
    _scrolledWindow.set_child(_columnView);
    _scrolledWindow.set_vexpand(true);
    _scrolledWindow.set_hexpand(true);

    applyPresentationSpec();
    applyColumnLayout();

    // Add to box (order: controls, scroll)
    append(_controlsBar);
    append(_scrolledWindow);
  }

  TrackViewPage::~TrackViewPage()
  {
    _queuedColumnLayoutUpdateConnection.disconnect();
  }

  void TrackViewPage::setupPresentationControls()
  {
    _controlsBar.set_spacing(8);
    _controlsBar.set_margin_start(4);
    _controlsBar.set_margin_end(4);
    _controlsBar.set_margin_top(4);
    _controlsBar.set_margin_bottom(4);

    _filterEntry.set_placeholder_text("Filter tracks...");
    _filterEntry.set_hexpand(true);
    _filterEntry.signal_changed().connect(sigc::mem_fun(*this, &TrackViewPage::onFilterChanged));

    _groupByLabel.set_text("Group");
    _groupByLabel.set_halign(Gtk::Align::START);
    _groupByLabel.set_valign(Gtk::Align::CENTER);

    _groupByOptions =
      Gtk::StringList::create({"None", "Artist", "Album", "Album Artist", "Genre", "Composer", "Work", "Year"});
    _groupByDropdown.set_model(_groupByOptions);
    _groupByDropdown.set_selected(dropdownPositionFor(_presentationSpec.groupBy));
    _groupByDropdown.property_selected().signal_changed().connect(
      sigc::mem_fun(*this, &TrackViewPage::onGroupByChanged));

    setupColumnControls();

    _controlsBar.append(_filterEntry);
    _controlsBar.append(_groupByLabel);
    _controlsBar.append(_groupByDropdown);
    _controlsBar.append(_columnsButton);
  }

  void TrackViewPage::setupColumnControls()
  {
    _columnsButton.set_label("Columns");
    _columnsButton.set_popover(_columnsPopover);

    _columnsPopoverBox.set_spacing(4);

    _columnsPopoverTitle.set_markup("<span size='small' weight='bold'>VISIBLE COLUMNS</span>");
    _columnsPopoverTitle.set_halign(Gtk::Align::START);
    _columnsPopoverTitle.add_css_class("dim-label");

    _columnToggleList.set_selection_mode(Gtk::SelectionMode::NONE);
    _columnToggleList.add_css_class("navigation-sidebar");

    _resetColumnsButton.set_label("Reset to Default");
    _resetColumnsButton.set_sensitive(true);
    _resetColumnsButton.add_css_class("suggested-action");
    _resetColumnsButton.signal_clicked().connect(
      [this]()
      {
        if (_columnLayoutModel)
        {
          _columnLayoutModel->reset();
        }
      });

    _columnsPopoverBox.append(_columnsPopoverTitle);
    _columnsPopoverBox.append(_columnToggleList);
    _columnsPopoverBox.append(_columnsPopoverSeparator);
    _columnsPopoverBox.append(_resetColumnsButton);
    _columnsPopover.set_child(_columnsPopoverBox);
  }

  void TrackViewPage::setupHeaderFactory()
  {
    _sectionHeaderFactory = Gtk::SignalListItemFactory::create();

    _sectionHeaderFactory->signal_setup_obj().connect(
      [](Glib::RefPtr<Glib::Object> const& object)
      {
        auto header = std::dynamic_pointer_cast<Gtk::ListHeader>(object);

        if (auto h = header; !h)
        {
          return;
        }

        auto* label = Gtk::make_managed<Gtk::Label>("");
        label->set_halign(Gtk::Align::START);
        label->set_margin_start(8);
        label->set_margin_end(8);
        label->set_margin_top(8);
        label->set_margin_bottom(4);
        label->set_xalign(0.0F);
        header->set_child(*label);
      });

    _sectionHeaderFactory->signal_bind_obj().connect(
      [this](Glib::RefPtr<Glib::Object> const& object)
      {
        auto header = std::dynamic_pointer_cast<Gtk::ListHeader>(object);
        auto* label = header ? dynamic_cast<Gtk::Label*>(header->get_child()) : nullptr;

        if (!header || !label)
        {
          return;
        }

        auto item = header->get_item();
        auto row = std::dynamic_pointer_cast<TrackRow>(item);

        if (!row)
        {
          label->set_text("");
          return;
        }

        auto text = groupLabelFor(row->getPresentationKeys(), _presentationSpec.groupBy);

        if (!text.empty())
        {
          text += " ";
        }

        text += "(" + trackCountLabel(header->get_n_items()) + ")";
        label->set_text(text);
      });
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
    // Style for error/info messages
    auto context = _statusLabel.get_style_context();
    context->add_class("dim-label");
  }

  void TrackViewPage::applyPresentationSpec()
  {
    updateColumnVisibility();

    if (_presentationSpec.sortBy.empty())
    {
      _sortModel->set_sorter(Glib::RefPtr<Gtk::Sorter>{});
    }
    else
    {
      _sortModel->set_sorter(
        createRowSorter([spec = _presentationSpec](TrackRow const& lhs, TrackRow const& rhs)
                        { return compareForSort(lhs.getPresentationKeys(), rhs.getPresentationKeys(), spec.sortBy); }));
    }

    if (_presentationSpec.groupBy == TrackGroupBy::None)
    {
      _sortModel->set_section_sorter(Glib::RefPtr<Gtk::Sorter>{});
      _columnView.set_header_factory(Glib::RefPtr<Gtk::ListItemFactory>{});
      return;
    }

    _sortModel->set_section_sorter(
      createRowSorter([groupBy = _presentationSpec.groupBy](TrackRow const& lhs, TrackRow const& rhs)
                      { return compareForGrouping(lhs.getPresentationKeys(), rhs.getPresentationKeys(), groupBy); }));
    _columnView.set_header_factory(_sectionHeaderFactory);
  }

  void TrackViewPage::setStatusMessage(std::string const& message)
  {
    if (message.empty())
    {
      clearStatusMessage();
      return;
    }

    _statusLabel.set_text(message);
    _statusLabel.set_visible(true);
  }

  void TrackViewPage::clearStatusMessage()
  {
    _statusLabel.set_text("");
    _statusLabel.set_visible(false);
  }

  void TrackViewPage::setupColumns()
  {
    _columns.reserve(trackColumnDefinitions().size());

    for (auto const& definition : trackColumnDefinitions())
    {
      auto title = Glib::ustring{std::string{definition.title}};
      auto column = Gtk::ColumnViewColumn::create(title, createTextColumnFactory(definition));
      column->set_id(Glib::ustring{std::string{definition.id}});
      column->set_expand(definition.expands);
      column->set_resizable(true);
      column->set_fixed_width(definition.defaultWidth);
      column->property_fixed_width().signal_changed().connect(
        [this]()
        {
          if (_syncingColumnLayout)
          {
            return;
          }

          queueSharedColumnLayoutUpdate();
        });
      _columnView.append_column(column);

      auto* toggle = Gtk::make_managed<Gtk::CheckButton>(title);
      toggle->signal_toggled().connect(
        [this, columnId = definition.column, toggleButton = toggle]()
        {
          if (_syncingColumnLayout || !_columnLayoutModel)
          {
            return;
          }

          auto layout = normalizeTrackColumnLayout(_columnLayoutModel->layout());
          for (auto& state : layout.columns)
          {
            if (state.column == columnId)
            {
              state.visible = toggleButton->get_active();
              break;
            }
          }

          _columnLayoutModel->setLayout(std::move(layout));
        });

      auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
      row->set_child(*toggle);
      row->set_activatable(false);
      _columnToggleList.append(*row);

      auto binding = ColumnBinding{};
      binding.id = definition.column;
      binding.column = column;
      binding.toggle = toggle;
      binding.defaultWidth = definition.defaultWidth;
      _columns.push_back(std::move(binding));
    }
  }

  void TrackViewPage::applyColumnLayout()
  {
    if (!_columnLayoutModel)
    {
      return;
    }

    auto const layout = normalizeTrackColumnLayout(_columnLayoutModel->layout());
    _syncingColumnLayout = true;

    for (std::size_t index = 0; index < layout.columns.size(); ++index)
    {
      auto const& state = layout.columns[index];
      auto* binding = findColumnBinding(state.column);

      if (!binding)
      {
        continue;
      }

      bool needsInsertion = true;

      if (_columnModel && _columnModel->get_n_items() > index)
      {
        auto object = _columnModel->get_object(static_cast<guint>(index));

        if (auto currentColumn = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(object);
            currentColumn && currentColumn->get_id() == binding->column->get_id())
        {
          needsInsertion = false;
        }
      }

      if (needsInsertion)
      {
        _columnView.insert_column(static_cast<guint>(index), binding->column);
      }

      auto const width = state.width == -1 ? binding->defaultWidth : state.width;

      if (binding->column->get_fixed_width() != width)
      {
        binding->column->set_fixed_width(width);
      }
    }

    syncColumnToggleStates();
    updateColumnVisibility();
    _syncingColumnLayout = false;
  }

  void TrackViewPage::syncColumnToggleStates()
  {
    if (!_columnLayoutModel)
    {
      return;
    }

    auto const layout = normalizeTrackColumnLayout(_columnLayoutModel->layout());
    for (auto const& state : layout.columns)
    {
      auto* binding = findColumnBinding(state.column);

      if (!binding || !binding->toggle)
      {
        continue;
      }

      binding->toggle->set_active(state.visible);
    }
  }

  void TrackViewPage::queueSharedColumnLayoutUpdate()
  {
    if (!_columnLayoutModel || _queuedColumnLayoutUpdateConnection.connected())
    {
      return;
    }

    // Reordering emits transient column-model updates while the dragged column is temporarily absent.
    // Capture the layout once GTK settles so newly enabled columns keep their persisted visibility.
    _queuedColumnLayoutUpdateConnection =
      Glib::signal_idle().connect(sigc::mem_fun(*this, &TrackViewPage::flushSharedColumnLayoutUpdate));
  }

  bool TrackViewPage::flushSharedColumnLayoutUpdate()
  {
    _queuedColumnLayoutUpdateConnection.disconnect();

    if (_syncingColumnLayout || !_columnLayoutModel)
    {
      return false;
    }

    updateSharedColumnLayout();
    return false;
  }

  void TrackViewPage::updateSharedColumnLayout()
  {
    if (_columnLayoutModel)
    {
      _capturingColumnLayout = true;
      _columnLayoutModel->setLayout(captureCurrentColumnLayout());
      _capturingColumnLayout = false;
    }
  }

  TrackColumnLayout TrackViewPage::captureCurrentColumnLayout() const
  {
    auto layout = TrackColumnLayout{};
    auto const currentLayout =
      _columnLayoutModel ? normalizeTrackColumnLayout(_columnLayoutModel->layout()) : defaultTrackColumnLayout();

    auto currentStateFor = [&currentLayout](TrackColumn column)
    {
      auto const it = std::find_if(currentLayout.columns.begin(),
                                   currentLayout.columns.end(),
                                   [column](TrackColumnState const& state) { return state.column == column; });
      return it != currentLayout.columns.end() ? *it : TrackColumnState{.column = column};
    };

    if (!_columnModel)
    {
      return currentLayout;
    }

    auto const nItems = _columnModel->get_n_items();
    layout.columns.reserve(nItems);

    for (guint i = 0; i < nItems; ++i)
    {
      auto object = _columnModel->get_object(i);
      auto column = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(object);

      if (!column)
      {
        continue;
      }

      auto const columnId = trackColumnFromId(std::string{column->get_id()});

      if (!columnId)
      {
        continue;
      }

      auto state = currentStateFor(*columnId);
      state.width = column->get_fixed_width();
      layout.columns.push_back(state);
    }

    return normalizeTrackColumnLayout(std::move(layout));
  }

  void TrackViewPage::updateColumnVisibility()
  {
    auto const layout =
      _columnLayoutModel ? normalizeTrackColumnLayout(_columnLayoutModel->layout()) : defaultTrackColumnLayout();

    for (auto const& state : layout.columns)
    {
      auto* binding = findColumnBinding(state.column);

      if (!binding)
      {
        continue;
      }

      binding->column->set_visible(state.visible && shouldShowColumn(_presentationSpec.groupBy, state.column));
    }
  }

  void TrackViewPage::onGroupByChanged()
  {
    _presentationSpec = presentationSpecForGroup(groupByFromDropdownPosition(_groupByDropdown.get_selected()));
    applyPresentationSpec();
  }

  void TrackViewPage::onFilterChanged()
  {
    auto filterText = _filterEntry.get_text();
    _adapter->setFilter(filterText);
  }

  void TrackViewPage::onSelectionChanged([[maybe_unused]] std::uint32_t position, [[maybe_unused]] std::uint32_t nItems)
  {
    _selectionChanged.emit();
  }

  std::size_t TrackViewPage::selectedTrackCount() const
  {
    auto count = std::size_t{0};
    auto model = _selectionModel->get_model();

    if (!model)
    {
      return count;
    }

    auto const nItems = model->get_n_items();

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      if (_selectionModel->is_selected(i))
      {
        ++count;
      }
    }

    return count;
  }

  std::optional<TrackViewPage::TrackId> TrackViewPage::trackIdAtPosition(std::uint32_t position) const
  {
    if (!_selectionModel)
    {
      return std::nullopt;
    }

    auto item = _selectionModel->get_object(position);

    if (!item)
    {
      return std::nullopt;
    }

    auto row = std::dynamic_pointer_cast<TrackRow>(item);

    if (!row)
    {
      return std::nullopt;
    }

    return row->getTrackId();
  }

  void TrackViewPage::selectTrack(TrackId trackId)
  {
    auto model = _selectionModel->get_model();

    if (!model)
    {
      return;
    }

    auto const nItems = model->get_n_items();

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      if (trackIdAtPosition(i) == trackId)
      {
        _selectionModel->select_item(i, true);
        _columnView.scroll_to(i, nullptr, Gtk::ListScrollFlags::FOCUS | Gtk::ListScrollFlags::SELECT, nullptr);
        break;
      }
    }
  }

  std::vector<TrackListAdapter::TrackId> TrackViewPage::getVisibleTrackIds() const
  {
    auto result = std::vector<TrackListAdapter::TrackId>{};

    auto model = _selectionModel->get_model();

    if (!model)
    {
      return result;
    }

    auto const nItems = model->get_n_items();
    result.reserve(nItems);

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      if (auto trackId = trackIdAtPosition(i))
      {
        result.push_back(*trackId);
      }
    }

    return result;
  }

  std::vector<TrackListAdapter::TrackId> TrackViewPage::getSelectedTrackIds() const
  {
    auto result = std::vector<TrackListAdapter::TrackId>{};

    auto model = _selectionModel->get_model();

    if (!model)
    {
      return result;
    }

    // Iterate through all items and check if selected
    auto nItems = model->get_n_items();

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      if (_selectionModel->is_selected(i))
      {
        if (auto trackId = trackIdAtPosition(i))
        {
          result.push_back(*trackId);
        }
      }
    }

    return result;
  }

  std::chrono::milliseconds TrackViewPage::getSelectedTracksDuration() const
  {
    auto totalDuration = std::chrono::milliseconds{0};

    auto model = _selectionModel->get_model();

    if (!model)
    {
      return std::chrono::milliseconds{0};
    }

    auto nItems = model->get_n_items();

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      if (_selectionModel->is_selected(i))
      {
        auto item = _selectionModel->get_object(i);

        if (auto row = std::dynamic_pointer_cast<TrackRow>(item))
        {
          totalDuration += row->getDuration();
        }
      }
    }

    return totalDuration;
  }

  sigc::signal<void()>& TrackViewPage::signalSelectionChanged()
  {
    return _selectionChanged;
  }

  sigc::signal<void(TrackViewPage::TrackId)>& TrackViewPage::signalTrackActivated()
  {
    return _trackActivated;
  }

  sigc::signal<void(double, double)>& TrackViewPage::signalContextMenuRequested()
  {
    return _contextMenuRequested;
  }

  sigc::signal<void(std::vector<TrackViewPage::TrackId>, double, double)>& TrackViewPage::signalTagEditRequested()
  {
    return _tagEditRequested;
  }

  void TrackViewPage::showTagPopover(TagPopover& popover, double x, double y)
  {
    auto rect = Gdk::Rectangle{static_cast<int>(x), static_cast<int>(y), 1, 1};
    popover.set_parent(_columnView);
    popover.set_pointing_to(rect);
    popover.popup();
  }

  void TrackViewPage::setupActivation()
  {
    _columnView.set_focusable(true);
    _columnView.set_focus_on_click(true);

    // Built-in activation carries the exact row position that GTK activated.
    _columnView.signal_activate().connect(
      [this](std::uint32_t position)
      {
        if (_suppressNextTrackActivation)
        {
          _suppressNextTrackActivation = false;
          return;
        }

        if (auto trackId = trackIdAtPosition(position))
        {
          _trackActivated.emit(*trackId);
          return;
        }

        onActivateCurrentSelection();
      });

    // Keep an explicit Enter handler so activation still works when GTK focus is
    // on the view but no activate action is emitted automatically.
    auto keyController = Gtk::EventControllerKey::create();
    keyController->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType modifiers)
      {
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
        {
          onActivateCurrentSelection();
          return true;
        }

        // Ctrl+T opens tag edit popover

        if (keyval == GDK_KEY_t || keyval == GDK_KEY_T)
        {
          if (bool(modifiers & Gdk::ModifierType::CONTROL_MASK))
          {
            if (auto selectedIds = getSelectedTrackIds(); !selectedIds.empty())
            {
              _tagEditRequested.emit(selectedIds, 0, 0);
            }

            return true;
          }
        }

        return false;
      },
      false);
    _columnView.add_controller(keyController);

    auto primaryClickController = Gtk::GestureClick::create();
    primaryClickController->set_button(GDK_BUTTON_PRIMARY);
    primaryClickController->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    primaryClickController->signal_pressed().connect(
      [this, primaryClickController](int nPress, double x, double y)
      {
        if (nPress != 2)
        {
          return;
        }

        if (auto* target = _columnView.pick(x, y, Gtk::PickFlags::NON_TARGETABLE); !isTagsCellWidget(target))
        {
          return;
        }

        auto selectedIds = getSelectedTrackIds();

        if (selectedIds.empty())
        {
          return;
        }

        primaryClickController->set_state(Gtk::EventSequenceState::CLAIMED);
        _suppressNextTrackActivation = true;
        _tagEditRequested.emit(selectedIds, x, y);
      });

    _columnView.add_controller(primaryClickController);

    auto secondaryClickController = Gtk::GestureClick::create();
    secondaryClickController->set_button(GDK_BUTTON_SECONDARY);
    secondaryClickController->signal_released().connect(
      [this](int, double x, double y)
      {
        if (selectedTrackCount() == 0)
        {
          return;
        }

        _contextMenuRequested.emit(x, y);
      });

    _columnView.add_controller(secondaryClickController);
  }

  void TrackViewPage::onActivateCurrentSelection()
  {
    if (_suppressNextTrackActivation)
    {
      _suppressNextTrackActivation = false;
      return;
    }

    if (auto trackId = getPrimarySelectedTrackId(); trackId)
    {
      _trackActivated.emit(*trackId);
    }
  }

  std::optional<TrackViewPage::TrackId> TrackViewPage::getPrimarySelectedTrackId() const
  {
    auto model = _selectionModel->get_model();

    if (!model)
    {
      return std::nullopt;
    }

    // Find first selected item
    auto nItems = model->get_n_items();

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      if (_selectionModel->is_selected(i))
      {
        if (auto trackId = trackIdAtPosition(i))
        {
          return trackId;
        }

        // Found first selected, no need to continue
        break;
      }
    }

    return std::nullopt;
  }

  TrackViewPage::ColumnBinding* TrackViewPage::findColumnBinding(TrackColumn column)
  {
    auto const it = std::find_if(
      _columns.begin(), _columns.end(), [column](ColumnBinding const& binding) { return binding.id == column; });
    return it != _columns.end() ? &*it : nullptr;
  }

  TrackViewPage::ColumnBinding const* TrackViewPage::findColumnBinding(TrackColumn column) const
  {
    auto const it = std::find_if(
      _columns.begin(), _columns.end(), [column](ColumnBinding const& binding) { return binding.id == column; });
    return it != _columns.end() ? &*it : nullptr;
  }

} // namespace app::ui

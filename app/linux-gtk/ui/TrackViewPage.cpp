// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackViewPage.h"
#include "LayoutConstants.h"
#include "TrackRow.h"
#include "TrackRowDataProvider.h"
#include <ao/utility/ByteView.h>
#include <ao/utility/Log.h>
#include <runtime/ViewService.h>

#include "ui/ThemeBus.h"
#include <cstdint>
#include <format>
#include <functional>
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
#include <memory>
#include <ranges>
#include <runtime/AppSession.h>
#include <runtime/EventBus.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>
#include <runtime/WorkspaceService.h>
#include <string>
#include <unordered_set>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    using RowCompareFn = std::move_only_function<int(TrackRow const&, TrackRow const&)>;
    constexpr auto kTagsCellWidgetName = "track-tags-cell";
    constexpr double kTitlePositionDivisor = 2.0;
    constexpr double kLongPressDelayFactor = 1.03;

    std::shared_ptr<Gdk::ContentProvider> createDragContentProvider(Gtk::Label* label, TrackColumn column)
    {
      auto const value = label->get_text().raw();

      if (value.empty())
      {
        return {};
      }

      auto prefix = std::string{};

      if (column == TrackColumn::Artist)
      {
        prefix = "$a=";
      }
      else if (column == TrackColumn::Album)
      {
        prefix = "$al=";
      }
      else if (column == TrackColumn::Genre)
      {
        prefix = "$g=";
      }

      if (prefix.empty())
      {
        return {};
      }

      auto const expr = prefix + "\"" + value + "\"";
      auto const bytes = Glib::Bytes::create(expr.data(), expr.size());

      return Gdk::ContentProvider::create("text/plain", bytes);
    }

    void ensureTrackPageCss(bool force = false)
    {
      static auto const provider = Gtk::CssProvider::create();
      static bool initialized = false;

      if (!initialized || force)
      {
        if (force)
        {
          if (auto const display = Gdk::Display::get_default(); display)
          {
            Gtk::StyleContext::remove_provider_for_display(display, provider);
          }
        }

        provider->load_from_data(R"(
          /* 1. The Dynamic Beam: Seamlessly following the Title column via CSS variables */
          columnview row.playing-row {
            /* We use var(--ao-title-x) which is updated in real-time by C++ */
            background-image: linear-gradient(to right, 
              transparent 0%, 
              alpha(@warning_bg_color, 0.2) var(--ao-title-x, 35%), 
              transparent 100%
            );
            background-color: transparent;
            border-color: transparent;
            transition: background-image 1.0s ease-out; /* Smooth sliding of the beam */
          }

          /* 2. Sharp Title Text */
          .playing-title {
            color: @theme_fg_color;
            font-weight: bold;
          }

          /* Sophisticated transition */
          columnview row {
            transition: all 450ms cubic-bezier(0.16, 1, 0.3, 1);
          }

          .inline-editor-stack { min-height: 0; margin: 0; }
          .inline-editor-label { border: 1px solid transparent; min-height: 0; }
          .inline-editor-entry { 
            background: @view_bg_color; 
            border: 1px solid @accent_color; 
            border-radius: 4px; 
            padding: 0 6px; 
            margin: 0; 
            min-height: 0;
            box-shadow: none; 
            font-weight: bold;
          }
          .inline-editor-entry text { padding-top: 0; padding-bottom: 0; min-height: 0; }
        )");

        if (!initialized || force)
        {
          if (auto const display = Gdk::Display::get_default(); display)
          {
            // Use USER priority to override potential theme/stylix overrides
            Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_USER);
          }

          initialized = true;
        }
      }
    }

    bool isTagsCellWidget(Gtk::Widget const* widget)
    {
      for (auto const* current = widget; current != nullptr; current = current->get_parent())
      {
        if (current->has_css_class(kTagsCellWidgetName))
        {
          return true;
        }
      }

      return false;
    }

    std::uint32_t dropdownPositionFor(ao::app::TrackGroupKey groupBy)
    {
      return static_cast<std::uint32_t>(groupBy);
    }

    ao::app::TrackGroupKey groupByFromDropdownPosition(std::uint32_t position)
    {
      if (position <= static_cast<std::uint32_t>(ao::app::TrackGroupKey::Year))
      {
        return static_cast<ao::app::TrackGroupKey>(position);
      }
      return ao::app::TrackGroupKey::None;
    }

    std::string trackCountLabel(::guint count)
    {
      auto label = std::format("{}", count);

      label += count == 1 ? " track" : " tracks";

      return label;
    }

    TrackRow* trackRowFromSorterItem(gpointer item)
    {
      auto* const object = static_cast<GObject*>(item);
      return object != nullptr ? dynamic_cast<TrackRow*>(Glib::wrap_auto(object, false)) : nullptr;
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

      Gtk::Ordering compare_vfunc(gpointer item1, gpointer item2) override
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
                               ao::app::AppSession& session,
                               ao::app::ViewId viewId)
    : Gtk::Box{Gtk::Orientation::VERTICAL}
    , _listId{listId}
    , _viewId{viewId}
    , _adapter{adapter}
    , _session{session}
    , _groupModel{Gtk::SortListModel::create(adapter.getModel(), Glib::RefPtr<Gtk::Sorter>{})}
    , _columnLayoutModel{columnLayoutModel}
  {
    ensureTrackPageCss();

    if (_viewId != ao::app::ViewId{})
    {
      _activeGroupBy = _session.views().trackListState(_viewId).groupBy;
    }

    // Subscribe to global theme refresh signal (e.g. triggered by SIGUSR1 or DBus in main.cpp)
    signalThemeRefresh().connect(
      [this]()
      {
        APP_LOG_INFO("Executing theme refresh for TrackViewPage...");

        // Now reload our custom CSS and update UI
        ensureTrackPageCss(true);
        updateTitlePositionVariable();
        _columnView.queue_draw();
      });

    _dynamicCssProvider = Gtk::CssProvider::create();
    _columnView.get_style_context()->add_provider(_dynamicCssProvider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Create multi-selection model to allow bulk operations
    _selectionModel = Gtk::MultiSelection::create(_groupModel);

    setupPresentationControls();
    setupStatusBar();
    setupHeaderFactory();

    // Set up column view
    _columnView.set_model(_selectionModel);
    updateSectionHeaders();
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
        [this](::guint, ::guint, ::guint)
        {
          if (_syncingColumnLayout)
          {
            return;
          }

          queueSharedColumnLayoutUpdate();
          updateTitlePositionVariable();
        });
    }

    _columnLayoutChangedConnection =
      _columnLayoutModel.signalChanged().connect(sigc::mem_fun(*this, &TrackViewPage::applyColumnLayout));

    // Set up activation (double-click, Enter key)
    setupActivation();

    // Set up scrolled window
    _scrolledWindow.set_child(_columnView);
    _scrolledWindow.set_vexpand(true);
    _scrolledWindow.set_hexpand(true);

    updateColumnVisibility();
    applyColumnLayout();
    updateFilterUi();

    // Add to box (order: controls, status, scroll)
    append(_controlsBar);
    append(_statusLabel);
    append(_scrolledWindow);

    updateTitlePositionVariable();
  }

  TrackViewPage::~TrackViewPage()
  {
    _queuedColumnLayoutUpdateConnection.disconnect();
  }

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
    _filterEntry.signal_changed().connect(sigc::mem_fun(*this, &TrackViewPage::onFilterChanged));

    _filterEntry.signal_icon_press().connect(
      [this](Gtk::Entry::IconPosition iconPosition)
      {
        if (iconPosition != Gtk::Entry::IconPosition::SECONDARY || _adapter.hasFilterError())
        {
          return;
        }

        if (auto const& expression = _adapter.currentSmartFilterExpression(); !expression.empty())
        {
          _createSmartListRequested.emit(expression);
        }
      });

    // Add drop target to support dragging attributes from the list
    auto const dropTarget = Gtk::DropTarget::create(Glib::Value<std::string>::value_type(), Gdk::DragAction::COPY);

    dropTarget->signal_drop().connect(
      [this](Glib::ValueBase const& value, double, double)
      {
        auto const holdsString = G_VALUE_HOLDS_STRING(value.gobj());

        if (holdsString)
        {
          auto val = Glib::Value<std::string>{};

          val.init(value.gobj());
          setFilterExpression(val.get());

          return true;
        }

        return false;
      },
      false);

    _filterEntry.add_controller(dropTarget);

    _groupByLabel.set_text("Group");
    _groupByLabel.set_halign(Gtk::Align::START);
    _groupByLabel.set_valign(Gtk::Align::CENTER);

    _groupByOptions =
      Gtk::StringList::create({"None", "Artist", "Album", "Album Artist", "Genre", "Composer", "Work", "Year"});
    _groupByDropdown.set_model(_groupByOptions);
    _groupByDropdown.set_selected(dropdownPositionFor(_activeGroupBy));
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

    _columnsPopoverBox.set_spacing(Layout::kSpacingSmall);

    _columnsPopoverTitle.set_markup("<span size='small' weight='bold'>VISIBLE COLUMNS</span>");
    _columnsPopoverTitle.set_halign(Gtk::Align::START);
    _columnsPopoverTitle.add_css_class("dim-label");

    _columnToggleList.set_selection_mode(Gtk::SelectionMode::NONE);
    _columnToggleList.add_css_class("navigation-sidebar");

    _resetColumnsButton.set_label("Reset to Default");
    _resetColumnsButton.set_sensitive(true);
    _resetColumnsButton.add_css_class("suggested-action");
    _resetColumnsButton.signal_clicked().connect([this]() { _columnLayoutModel.reset(); });

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
    auto const groupBy = proj != nullptr ? proj->presentation().groupBy : ao::app::TrackGroupKey::None;

    if (groupBy == ao::app::TrackGroupKey::None)
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

    // Style for error/info messages
    auto const context = _statusLabel.get_style_context();

    context->add_class("dim-label");
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
      auto const title = Glib::ustring{std::string{definition.title}};
      auto const column = Gtk::ColumnViewColumn::create(title, createTextColumnFactory(definition));

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

      _columnNotifyConnections.push_back(column->property_fixed_width().signal_changed().connect(
        sigc::mem_fun(*this, &TrackViewPage::updateTitlePositionVariable)));

      _columnView.append_column(column);

      auto* const toggle = Gtk::make_managed<Gtk::CheckButton>(title);

      toggle->signal_toggled().connect(
        [this, columnId = definition.column, toggleButton = toggle]()
        {
          if (_syncingColumnLayout)
          {
            return;
          }

          auto layout = normalizeTrackColumnLayout(_columnLayoutModel.layout());

          for (auto& state : layout.columns)
          {
            if (state.column == columnId)
            {
              state.visible = toggleButton->get_active();
              break;
            }
          }

          _columnLayoutModel.setLayout(layout);
        });

      auto* const row = Gtk::make_managed<Gtk::ListBoxRow>();

      row->set_child(*toggle);
      row->set_activatable(false);
      _columnToggleList.append(*row);

      auto binding = ColumnBinding{
        .id = definition.column, .column = column, .toggle = toggle, .defaultWidth = definition.defaultWidth};

      _columns.push_back(std::move(binding));
    }
  }

  void TrackViewPage::applyColumnLayout()
  {
    auto const layout = normalizeTrackColumnLayout(_columnLayoutModel.layout());

    _syncingColumnLayout = true;

    for (std::size_t index = 0; index < layout.columns.size(); ++index)
    {
      auto const& state = layout.columns[index];
      auto* const binding = findColumnBinding(state.column);

      if (binding == nullptr)
      {
        continue;
      }

      bool needsInsertion = true;

      if (_columnModel && _columnModel->get_n_items() > index)
      {
        auto const object = _columnModel->get_object(static_cast<::guint>(index));

        if (auto const currentColumn = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(object);
            currentColumn && currentColumn->get_id() == binding->column->get_id())
        {
          needsInsertion = false;
        }
      }

      if (needsInsertion)
      {
        _columnView.insert_column(static_cast<::guint>(index), binding->column);
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
    auto const layout = normalizeTrackColumnLayout(_columnLayoutModel.layout());

    for (auto const& state : layout.columns)
    {
      auto* const binding = findColumnBinding(state.column);

      if (binding == nullptr || binding->toggle == nullptr)
      {
        continue;
      }

      binding->toggle->set_active(state.visible);
    }
  }

  void TrackViewPage::queueSharedColumnLayoutUpdate()
  {
    if (_queuedColumnLayoutUpdateConnection.connected())
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

    if (_syncingColumnLayout)
    {
      return false;
    }

    updateSharedColumnLayout();

    return false;
  }

  void TrackViewPage::updateSharedColumnLayout()
  {
    _capturingColumnLayout = true;
    _columnLayoutModel.setLayout(captureCurrentColumnLayout());
    _capturingColumnLayout = false;
  }

  TrackColumnLayout TrackViewPage::captureCurrentColumnLayout() const
  {
    auto layout = TrackColumnLayout{};
    auto currentLayout = normalizeTrackColumnLayout(_columnLayoutModel.layout());

    auto const currentStateFor = [&currentLayout](TrackColumn column)
    {
      auto const it = std::ranges::find(currentLayout.columns, column, &TrackColumnState::column);

      return it != currentLayout.columns.end() ? *it : TrackColumnState{.column = column};
    };

    if (!_columnModel)
    {
      return currentLayout;
    }

    auto const nItems = _columnModel->get_n_items();

    layout.columns.reserve(nItems);

    for (::guint i = 0; i < nItems; ++i)
    {
      auto const object = _columnModel->get_object(i);
      auto const column = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(object);

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

    return normalizeTrackColumnLayout(layout);
  }

  void TrackViewPage::updateColumnVisibility()
  {
    auto const layout = normalizeTrackColumnLayout(_columnLayoutModel.layout());

    auto redundantColumns = std::unordered_set<TrackColumn>{};
    if (auto* proj = _adapter.projection())
    {
      for (auto const& field : proj->presentation().redundantFields)
      {
        if (auto col = redundantFieldToColumn(field))
        {
          redundantColumns.insert(*col);
        }
      }
    }

    for (auto const& state : layout.columns)
    {
      auto* const binding = findColumnBinding(state.column);

      if (binding == nullptr)
      {
        continue;
      }

      bool const hiddenByGroup = redundantColumns.contains(state.column);
      binding->column->set_visible(state.visible && !hiddenByGroup);
    }
  }

  void TrackViewPage::onGroupByChanged()
  {
    _activeGroupBy = groupByFromDropdownPosition(_groupByDropdown.get_selected());

    if (_viewId != ao::app::ViewId{})
    {
      _session.views().setGrouping(_viewId, _activeGroupBy);
    }

    updateSectionHeaders();
    updateColumnVisibility();
  }

  void TrackViewPage::onFilterChanged()
  {
    auto const filterText = _filterEntry.get_text();

    _adapter.setFilter(filterText);
    updateFilterUi();
  }

  void TrackViewPage::setFilterExpression(std::string const& expression)
  {
    _filterEntry.set_text(expression);
  }

  void TrackViewPage::updateFilterUi()
  {
    auto const hasExpressionError = _adapter.filterMode() == TrackFilterMode::Expression && _adapter.hasFilterError();

    if (hasExpressionError)
    {
      _filterEntry.add_css_class("error");
      setStatusMessage("Expression error: " + _adapter.filterErrorMessage());
    }
    else
    {
      _filterEntry.remove_css_class("error");
      clearStatusMessage();
    }

    auto const canCreateSmartList = !_adapter.currentSmartFilterExpression().empty() && !_adapter.hasFilterError();

    _filterEntry.set_icon_sensitive(Gtk::Entry::IconPosition::SECONDARY, canCreateSmartList);
  }

  void TrackViewPage::onSelectionChanged(std::uint32_t /*position*/, std::uint32_t /*nItems*/)
  {
    _selectionChanged.emit();
  }

  std::size_t TrackViewPage::selectedTrackCount() const
  {
    auto count = std::size_t{0};
    auto const model = _selectionModel->get_model();

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

    auto const item = _selectionModel->get_object(position);

    if (!item)
    {
      return std::nullopt;
    }

    auto const row = std::dynamic_pointer_cast<TrackRow>(item);

    if (!row)
    {
      return std::nullopt;
    }

    return row->getTrackId();
  }

  void TrackViewPage::selectTrack(TrackId trackId)
  {
    auto const model = _selectionModel->get_model();

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

  void TrackViewPage::setPlayingTrackId(std::optional<TrackId> trackId)
  {
    auto const model = _selectionModel->get_model();

    if (!model)
    {
      return;
    }

    auto const nItems = model->get_n_items();

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      auto const item = model->get_object(i);
      auto const row = std::dynamic_pointer_cast<TrackRow>(item);

      if (row)
      {
        bool const isPlaying = trackId && row->getTrackId() == *trackId;

        if (row->isPlaying() != isPlaying)
        {
          row->setPlaying(isPlaying);
        }
      }
    }
  }

  std::vector<TrackListAdapter::TrackId> TrackViewPage::getVisibleTrackIds() const
  {
    auto result = std::vector<TrackListAdapter::TrackId>{};
    auto const model = _selectionModel->get_model();

    if (!model)
    {
      return result;
    }

    auto const nItems = model->get_n_items();

    result.reserve(nItems);

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      if (auto const trackId = trackIdAtPosition(i))
      {
        result.push_back(*trackId);
      }
    }

    return result;
  }

  std::vector<TrackListAdapter::TrackId> TrackViewPage::getSelectedTrackIds() const
  {
    auto result = std::vector<TrackListAdapter::TrackId>{};
    auto const model = _selectionModel->get_model();

    if (!model)
    {
      return result;
    }

    // Iterate through all items and check if selected
    auto const nItems = model->get_n_items();

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      if (_selectionModel->is_selected(i))
      {
        if (auto const trackId = trackIdAtPosition(i))
        {
          result.push_back(*trackId);
        }
      }
    }

    return result;
  }

  std::vector<Glib::RefPtr<TrackRow>> TrackViewPage::getSelectedRows() const
  {
    auto result = std::vector<Glib::RefPtr<TrackRow>>{};
    auto const model = _selectionModel->get_model();

    if (!model)
    {
      return result;
    }

    auto const nItems = model->get_n_items();

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      if (_selectionModel->is_selected(i))
      {
        auto item = model->get_object(i);

        if (auto row = std::dynamic_pointer_cast<TrackRow>(item))
        {
          result.push_back(std::move(row));
        }
      }
    }

    return result;
  }

  std::chrono::milliseconds TrackViewPage::getSelectedTracksDuration() const
  {
    auto totalDuration = std::chrono::milliseconds{0};
    auto const model = _selectionModel->get_model();

    if (!model)
    {
      return std::chrono::milliseconds{0};
    }

    auto const nItems = model->get_n_items();

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      if (_selectionModel->is_selected(i))
      {
        auto const item = _selectionModel->get_object(i);

        if (auto const row = std::dynamic_pointer_cast<TrackRow>(item))
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

  TrackViewPage::TagEditRequestedSignal& TrackViewPage::signalTagEditRequested()
  {
    return _tagEditRequested;
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

        if (auto const trackId = trackIdAtPosition(position))
        {
          _trackActivated.emit(*trackId);
          return;
        }

        onActivateCurrentSelection();
      });

    // Keep an explicit Enter handler so activation still works when GTK focus is
    // on the view but no activate action is emitted automatically.
    auto const keyController = Gtk::EventControllerKey::create();

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
          if (static_cast<bool>(modifiers & Gdk::ModifierType::CONTROL_MASK))
          {
            if (auto const selectedIds = getSelectedTrackIds(); !selectedIds.empty())
            {
              _tagEditRequested.emit(selectedIds, nullptr);
            }

            return true;
          }
        }

        return false;
      },
      false);

    _columnView.add_controller(keyController);

    auto const primaryClickController = Gtk::GestureClick::create();

    primaryClickController->set_button(GDK_BUTTON_PRIMARY);
    primaryClickController->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);

    primaryClickController->signal_pressed().connect(
      [this, primaryClickController](int nPress, double xPos, double yPos)
      {
        if (nPress != 2)
        {
          return;
        }

        auto* const target = _columnView.pick(xPos, yPos, Gtk::PickFlags::NON_TARGETABLE);

        if (!isTagsCellWidget(target))
        {
          return;
        }

        auto const selectedIds = getSelectedTrackIds();

        if (selectedIds.empty())
        {
          return;
        }

        primaryClickController->set_state(Gtk::EventSequenceState::CLAIMED);
        _suppressNextTrackActivation = true;
        _tagEditRequested.emit(selectedIds, dynamic_cast<Gtk::Widget*>(target));
      });

    _columnView.add_controller(primaryClickController);

    auto const secondaryClickController = Gtk::GestureClick::create();

    secondaryClickController->set_button(GDK_BUTTON_SECONDARY);

    secondaryClickController->signal_released().connect(
      [this, secondaryClickController](int, double xPos, double yPos)
      {
        if (selectedTrackCount() == 0)
        {
          return;
        }

        _contextMenuRequested.emit(xPos, yPos);
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

    if (auto const trackId = getPrimarySelectedTrackId(); trackId)
    {
      _trackActivated.emit(*trackId);
    }
  }

  std::optional<TrackViewPage::TrackId> TrackViewPage::getPrimarySelectedTrackId() const
  {
    auto const model = _selectionModel->get_model();

    if (!model)
    {
      return std::nullopt;
    }

    // Find first selected item
    auto const nItems = model->get_n_items();

    for (std::uint32_t i = 0; i < nItems; ++i)
    {
      if (_selectionModel->is_selected(i))
      {
        if (auto const trackId = trackIdAtPosition(i))
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
    auto const it = std::ranges::find(_columns, column, &ColumnBinding::id);

    return it != _columns.end() ? &*it : nullptr;
  }

  TrackViewPage::ColumnBinding const* TrackViewPage::findColumnBinding(TrackColumn column) const
  {
    auto const it = std::ranges::find(_columns, column, &ColumnBinding::id);

    return it != _columns.end() ? &*it : nullptr;
  }

  void TrackViewPage::updateTitlePositionVariable()
  {
    double titleX = 0;
    bool found = false;

    auto const columns = _columnView.get_columns();

    if (!columns)
    {
      return;
    }

    for (std::uint32_t i = 0; i < columns->get_n_items(); ++i)
    {
      auto const col = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columns->get_object(i));

      if (!col || !col->get_visible())
      {
        continue;
      }

      if (col->get_title() == "Title")
      {
        titleX += col->get_fixed_width() / kTitlePositionDivisor;
        found = true;
        break;
      }

      titleX += col->get_fixed_width();
    }

    if (found)
    {
      auto const css = std::format("columnview {{ --ao-title-x: {:.1f}px; }}", titleX);

      _dynamicCssProvider->load_from_data(css);
    }
  }

  void TrackViewPage::onTextColumnSetup(Glib::RefPtr<Gtk::ListItem> const& listItem,
                                        TrackColumnDefinition const& definition,
                                        bool isEditable,
                                        bool isHyperlink)
  {
    if (definition.tagsCell)
    {
      auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);

      box->set_halign(Gtk::Align::FILL);
      box->set_hexpand(true);
      box->add_css_class(kTagsCellWidgetName);

      auto* const label = Gtk::make_managed<Gtk::Label>("");

      label->set_halign(Gtk::Align::START);
      label->set_ellipsize(Pango::EllipsizeMode::END);
      label->set_hexpand(true);

      box->append(*label);
      listItem->set_child(*box);

      return;
    }

    if (isEditable)
    {
      auto* const stack = Gtk::make_managed<Gtk::Stack>();

      stack->add_css_class("inline-editor-stack");
      stack->set_transition_type(Gtk::StackTransitionType::CROSSFADE);
      stack->set_vhomogeneous(false);
      stack->set_hexpand(true);
      stack->set_vexpand(true);
      stack->set_halign(Gtk::Align::FILL);
      stack->set_valign(Gtk::Align::FILL);

      auto* const label = Gtk::make_managed<Gtk::Label>("");

      label->set_halign(Gtk::Align::START);
      label->set_ellipsize(Pango::EllipsizeMode::END);
      label->set_hexpand(true);
      label->add_css_class("inline-editor-label");

      if (isHyperlink)
      {
        auto const source = Gtk::DragSource::create();

        source->signal_prepare().connect(
          sigc::slot<std::shared_ptr<Gdk::ContentProvider>(double, double)>(
            [label, column = definition.column](double, double) -> std::shared_ptr<Gdk::ContentProvider>
            { return createDragContentProvider(label, column); }),
          false);

        label->add_controller(source);
      }

      stack->add(*label, "display");

      auto* const entry = Gtk::make_managed<Gtk::Entry>();

      entry->add_css_class("inline-editor-entry");
      entry->set_hexpand(true);
      entry->set_vexpand(true);
      entry->set_halign(Gtk::Align::FILL);
      entry->set_valign(Gtk::Align::FILL);

      stack->add(*entry, "edit");

      auto const longPress = Gtk::GestureLongPress::create();

      longPress->set_delay_factor(kLongPressDelayFactor);
      longPress->signal_pressed().connect(
        [stack, entry](double, double)
        {
          stack->set_visible_child("edit");
          entry->grab_focus();
          entry->select_region(0, -1);
        });

      stack->add_controller(longPress);

      listItem->set_child(*stack);

      return;
    }

    auto* const label = Gtk::make_managed<Gtk::Label>("");

    label->set_halign(definition.numeric ? Gtk::Align::END : Gtk::Align::START);
    label->set_xalign(definition.numeric ? 1.0F : 0.0F);

    if (!definition.numeric)
    {
      label->set_ellipsize(Pango::EllipsizeMode::END);
    }

    if (isHyperlink)
    {
      auto const source = Gtk::DragSource::create();

      source->signal_prepare().connect(
        sigc::slot<std::shared_ptr<Gdk::ContentProvider>(double, double)>(
          [label, column = definition.column](double, double) -> std::shared_ptr<Gdk::ContentProvider>
          { return createDragContentProvider(label, column); }),
        false);

      label->add_controller(source);
    }

    listItem->set_child(*label);
  }

  void TrackViewPage::onTextColumnBind(Glib::RefPtr<Gtk::ListItem> const& listItem,
                                       TrackColumnDefinition const& definition,
                                       bool isEditable)
  {
    auto const item = listItem->get_item();
    auto const row = std::dynamic_pointer_cast<TrackRow>(item);

    if (row == nullptr)
    {
      return;
    }

    if (definition.tagsCell)
    {
      auto* const box = dynamic_cast<Gtk::Box*>(listItem->get_child());
      auto* const label = (box != nullptr) ? dynamic_cast<Gtk::Label*>(box->get_first_child()) : nullptr;

      if (label != nullptr)
      {
        label->set_text(row->getColumnText(definition.column));
      }

      // Fall through to updateStyles
    }
    else if (isEditable)
    {
      onTextColumnBindEditable(listItem, definition, row);
    }
    else
    {
      onTextColumnBindStatic(listItem, definition, row);
    }

    auto const updateStyles = [listItem, row, definition]()
    {
      auto* const child = listItem->get_child();

      if (row->isPlaying())
      {
        if (auto* const cell = child->get_parent())
        {
          if (auto* const rowWidget = cell->get_parent())
          {
            rowWidget->add_css_class("playing-row");
          }

          if (definition.column == TrackColumn::Title)
          {
            cell->add_css_class("playing-title");
          }
          else
          {
            child->add_css_class("playing-dim");
          }
        }
      }
      else
      {
        if (auto* const cell = child->get_parent())
        {
          if (auto* const rowWidget = cell->get_parent())
          {
            rowWidget->remove_css_class("playing-row");
          }

          cell->remove_css_class("playing-title");
        }

        child->remove_css_class("playing-dim");
      }
    };

    updateStyles();

    auto const connection = row->property_playing().signal_changed().connect(updateStyles);

    listItem->set_data("playing-connection",
                       new sigc::connection{connection}, // NOLINT(cppcoreguidelines-owning-memory)
                       [](::gpointer data)
                       {
                         auto* const conn =
                           static_cast<sigc::connection*>(data); // NOLINT(cppcoreguidelines-owning-memory)

                         conn->disconnect();
                         delete conn; // NOLINT(cppcoreguidelines-owning-memory)
                       });
  }

  void TrackViewPage::onTextColumnBindEditable(Glib::RefPtr<Gtk::ListItem> const& listItem,
                                               TrackColumnDefinition const& definition,
                                               Glib::RefPtr<TrackRow> const& row)
  {
    auto* const stack = dynamic_cast<Gtk::Stack*>(listItem->get_child());
    auto* const label = (stack != nullptr) ? dynamic_cast<Gtk::Label*>(stack->get_child_by_name("display")) : nullptr;
    auto* const entry = (stack != nullptr) ? dynamic_cast<Gtk::Entry*>(stack->get_child_by_name("edit")) : nullptr;

    if (label != nullptr && entry != nullptr)
    {
      label->set_text(row->getColumnText(definition.column));
      entry->set_text(row->getColumnText(definition.column));

      auto const commitChange = [this, stack, entry, row, column = definition.column]()
      { commitMetadataChange(stack, entry, row, column); };

      auto const cancelChange = [stack, row, definition, entry]()
      {
        stack->set_visible_child("display");
        entry->set_text(row->getColumnText(definition.column));
      };

      auto const activateConn = entry->signal_activate().connect(commitChange);
      auto const focusController = Gtk::EventControllerFocus::create();

      focusController->signal_leave().connect([commitChange]() { commitChange(); });
      entry->add_controller(focusController);

      auto const keyController = Gtk::EventControllerKey::create();

      keyController->signal_key_pressed().connect(
        [cancelChange](guint keyval, guint, Gdk::ModifierType) -> bool
        {
          if (keyval == GDK_KEY_Escape)
          {
            cancelChange();
            return true;
          }
          return false;
        },
        false);
      entry->add_controller(keyController);

      auto modelConnection = sigc::connection{};

      if (definition.column == TrackColumn::Title)
      {
        modelConnection = row->property_title().signal_changed().connect(
          [label, entry, row]()
          {
            label->set_text(row->getTitle());
            entry->set_text(row->getTitle());
          });
      }
      else if (definition.column == TrackColumn::Artist)
      {
        modelConnection = row->property_artist().signal_changed().connect(
          [label, entry, row]()
          {
            label->set_text(row->getArtist());
            entry->set_text(row->getArtist());
          });
      }
      else if (definition.column == TrackColumn::Album)
      {
        modelConnection = row->property_album().signal_changed().connect(
          [label, entry, row]()
          {
            label->set_text(row->getAlbum());
            entry->set_text(row->getAlbum());
          });
      }

      listItem->set_data("model-connection",
                         new sigc::connection{modelConnection}, // NOLINT(cppcoreguidelines-owning-memory)
                         [](::gpointer data)
                         {
                           auto* const conn =
                             static_cast<sigc::connection*>(data); // NOLINT(cppcoreguidelines-owning-memory)

                           conn->disconnect();
                           delete conn; // NOLINT(cppcoreguidelines-owning-memory)
                         });

      listItem->set_data("activate-connection",
                         new sigc::connection{activateConn}, // NOLINT(cppcoreguidelines-owning-memory)
                         [](::gpointer data)
                         {
                           auto* const conn =
                             static_cast<sigc::connection*>(data); // NOLINT(cppcoreguidelines-owning-memory)

                           conn->disconnect();
                           delete conn; // NOLINT(cppcoreguidelines-owning-memory)
                         });
    }
  }

  void TrackViewPage::onTextColumnBindStatic(Glib::RefPtr<Gtk::ListItem> const& listItem,
                                             TrackColumnDefinition const& definition,
                                             Glib::RefPtr<TrackRow> const& row)
  {
    auto* const label = dynamic_cast<Gtk::Label*>(listItem->get_child());

    if (label != nullptr)
    {
      label->set_text(row->getColumnText(definition.column));
    }
  }

  void TrackViewPage::commitMetadataChange(Gtk::Stack* stack,
                                           Gtk::Entry* entry,
                                           Glib::RefPtr<TrackRow> const& row,
                                           TrackColumn column)
  {
    if (stack->get_visible_child_name() != "edit")
    {
      return;
    }

    auto const newValue = entry->get_text().raw();
    auto const oldValue = row->getColumnText(column).raw();

    stack->set_visible_child("display");

    if (newValue == oldValue)
    {
      return;
    }

    auto patch = ao::app::MetadataPatch{};

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

  Glib::RefPtr<Gtk::SignalListItemFactory> TrackViewPage::createTextColumnFactory(
    TrackColumnDefinition const& definition)
  {
    auto const isEditable = definition.column == TrackColumn::Title || definition.column == TrackColumn::Artist ||
                            definition.column == TrackColumn::Album;
    auto const isHyperlink = definition.column == TrackColumn::Artist || definition.column == TrackColumn::Album ||
                             definition.column == TrackColumn::Genre;

    auto const factory = Gtk::SignalListItemFactory::create();

    factory->signal_setup().connect(
      [this, definition, isEditable, isHyperlink](Glib::RefPtr<Gtk::ListItem> const& listItem)
      { onTextColumnSetup(listItem, definition, isEditable, isHyperlink); });

    factory->signal_bind().connect([this, definition, isEditable](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                   { onTextColumnBind(listItem, definition, isEditable); });

    factory->signal_unbind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        listItem->set_data("playing-connection", nullptr);
        listItem->set_data("activate-connection", nullptr);
        listItem->set_data("model-connection", nullptr);
      });

    return factory;
  }
} // namespace ao::gtk

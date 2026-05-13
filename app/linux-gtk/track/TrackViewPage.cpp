// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackViewPage.h"

#include "layout/LayoutConstants.h"
#include "track/TrackFilterController.h"
#include "track/TrackRowObject.h"
#include "track/TrackRowCache.h"
#include "track/TrackSelectionController.h"
#include "shell/ThemeBus.h"
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

      auto const expr = std::format("{}\"{}\"", prefix, value);
      auto const bytes = Glib::Bytes::create(expr.data(), expr.size());

      return Gdk::ContentProvider::create("text/plain", bytes);
    }

    void ensureTrackPageCss(bool force = false)
    {
      static auto const provider = Gtk::CssProvider::create();
      static bool initialized = false;

      if (initialized && !force)
      {
        return;
      }

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

      if (auto const display = Gdk::Display::get_default(); display)
      {
        Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_USER);
      }

      initialized = true;
    }

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
    ensureTrackPageCss();

    if (_viewId != ao::rt::ViewId{})
    {
      _activeGroupBy = _session.views().trackListState(_viewId).groupBy;
    }

    _selectionModel = Gtk::MultiSelection::create(_groupModel);

    _filterController = std::make_unique<TrackFilterController>(_session, _viewId, _filterEntry);
    _filterController->setStatusMessageCallback(
      [this](std::string_view msg) { setStatusMessage(msg); });
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
        ensureTrackPageCss(true);
        _columnController->updateTitlePositionVariable();
        _columnView.queue_draw();
      });

    _columnView.get_style_context()->add_provider(_columnController->cssProvider(), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    setupPresentationControls();
    setupStatusBar();
    setupHeaderFactory();

    _columnView.set_model(_selectionModel);
    updateSectionHeaders();
    _contextPopover.set_has_arrow(false);
    _contextPopover.set_parent(_columnView);

    _columnView.set_show_row_separators(true);
    _columnView.set_reorderable(true);

    _columnController->setupColumns(
      [this](TrackColumnDefinition const& def) { return createTextColumnFactory(def); });
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
      box->add_css_class("track-tags-cell");

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
    auto const row = std::dynamic_pointer_cast<TrackRowObject>(item);

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
    }
    else if (isEditable)
    {
      onTextColumnBindEditable(listItem, definition, row);
    }
    else
    {
      onTextColumnBindStatic(listItem, definition, row);
    }

    auto const updateStyles = [listItem, row, definition]
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
                                               Glib::RefPtr<TrackRowObject> const& row)
  {
    auto* const stack = dynamic_cast<Gtk::Stack*>(listItem->get_child());
    auto* const label = (stack != nullptr) ? dynamic_cast<Gtk::Label*>(stack->get_child_by_name("display")) : nullptr;
    auto* const entry = (stack != nullptr) ? dynamic_cast<Gtk::Entry*>(stack->get_child_by_name("edit")) : nullptr;

    if (label != nullptr && entry != nullptr)
    {
      label->set_text(row->getColumnText(definition.column));
      entry->set_text(row->getColumnText(definition.column));

      auto const commitChange = [this, stack, entry, row, column = definition.column]
      { commitMetadataChange(stack, entry, row, column); };

      auto const cancelChange = [stack, row, definition, entry]
      {
        stack->set_visible_child("display");
        entry->set_text(row->getColumnText(definition.column));
      };

      auto const activateConn = entry->signal_activate().connect(commitChange);
      auto const focusController = Gtk::EventControllerFocus::create();

      focusController->signal_leave().connect([commitChange] { commitChange(); });
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
          [label, entry, row]
          {
            label->set_text(row->getTitle());
            entry->set_text(row->getTitle());
          });
      }
      else if (definition.column == TrackColumn::Artist)
      {
        modelConnection = row->property_artist().signal_changed().connect(
          [label, entry, row]
          {
            label->set_text(row->getArtist());
            entry->set_text(row->getArtist());
          });
      }
      else if (definition.column == TrackColumn::Album)
      {
        modelConnection = row->property_album().signal_changed().connect(
          [label, entry, row]
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
                                             Glib::RefPtr<TrackRowObject> const& row)
  {
    auto* const label = dynamic_cast<Gtk::Label*>(listItem->get_child());

    if (label != nullptr)
    {
      label->set_text(row->getColumnText(definition.column));
    }
  }

  void TrackViewPage::commitMetadataChange(Gtk::Stack* stack,
                                           Gtk::Entry* entry,
                                           Glib::RefPtr<TrackRowObject> const& row,
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

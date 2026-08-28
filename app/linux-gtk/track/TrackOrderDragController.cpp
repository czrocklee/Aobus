// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "track/TrackOrderDragController.h"

#include "common/AccessibleLabel.h"
#include "common/UiWorkflow.h"
#include "i18n/GtkTextCatalog.h"
#include "track/TrackRowBinding.h"
#include "track/TrackRowObject.h"
#include "track/TrackSelectionController.h"
#include <ao/CoreIds.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/list/ListOrder.h>
#include <ao/uimodel/library/list/ListOrderSession.h>
#include <ao/uimodel/presentation/PresentationText.h>

#include <gdkmm/contentprovider.h>
#include <gdkmm/drag.h>
#include <gdkmm/enums.h>
#include <gdkmm/graphene_point.h>
#include <glib-object.h>
#include <glib.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/enums.h>
#include <gtkmm/image.h>
#include <gtkmm/listitem.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr auto kDropBeforeCssClass = "ao-order-drop-before";
    constexpr auto kDropAfterCssClass = "ao-order-drop-after";
    constexpr auto kInvalidListPosition = std::numeric_limits<guint>::max();
    constexpr double kAutoScrollEdge = 36.0;
    constexpr double kAutoScrollStep = 18.0;
    constexpr int kDragHandleColumnWidth = 36;

    class DragTokenCounter final
    {
    public:
      std::uint64_t next() const noexcept { return _next.fetch_add(1); }

    private:
      mutable std::atomic<std::uint64_t> _next{1};
    };

    std::uint64_t nextDragToken() noexcept
    {
      static auto const counter = DragTokenCounter{};
      return counter.next();
    }

    std::optional<std::string> stringFromDropValue(Glib::ValueBase const& value)
    {
      if (value.gobj()->g_type != G_TYPE_STRING)
      {
        return std::nullopt;
      }

      auto stringValue = Glib::Value<std::string>{};
      stringValue.init(value.gobj());
      return stringValue.get();
    }

    Glib::RefPtr<Gdk::ContentProvider> stringContentProvider(std::string const& value)
    {
      auto stringValue = Glib::Value<std::string>{};
      stringValue.init(Glib::Value<std::string>::value_type());
      stringValue.set(value);
      return Gdk::ContentProvider::create(stringValue);
    }

    Gtk::Widget* rowWidgetFor(Gtk::Widget& widget)
    {
      for (auto* current = &widget; current != nullptr; current = current->get_parent())
      {
        if (current->get_css_name() == "row")
        {
          return current;
        }
      }

      return nullptr;
    }
  } // namespace

  struct TrackOrderDragController::State final : std::enable_shared_from_this<State>
  {
    State(rt::AppRuntime& runtimeValue,
          rt::ViewId const viewIdValue,
          i18n::MessageCatalog textCatalogValue,
          Gtk::ScrolledWindow& scrolledWindowValue,
          TrackSelectionController& selectionControllerValue,
          Callbacks callbacksValue)
      : runtime{runtimeValue}
      , viewId{viewIdValue}
      , textCatalog{std::move(textCatalogValue)}
      , scrolledWindow{scrolledWindowValue}
      , selectionController{selectionControllerValue}
      , callbacks{std::move(callbacksValue)}
    {
    }

    void showStatus(std::string message) const
    {
      if (!closing && callbacks.onStatus)
      {
        callbacks.onStatus(std::move(message));
      }
    }

    void clearStatus() const
    {
      if (!closing && callbacks.onClearStatus)
      {
        callbacks.onClearStatus();
      }
    }

    void clearIndicator()
    {
      if (indicatorRow != nullptr)
      {
        indicatorRow->remove_css_class(kDropBeforeCssClass);
        indicatorRow->remove_css_class(kDropAfterCssClass);
        indicatorRow = nullptr;
      }
    }

    void clearActiveDrag()
    {
      clearIndicator();
      invalidatedSubscription.reset();
      sessionPtr.reset();
      selectedTrackIds.clear();
      token.clear();
      invalidated = false;
    }

    void close()
    {
      closing = true;
      tasks.cancelAll();
      clearActiveDrag();
    }

    Glib::RefPtr<Gdk::ContentProvider> beginDrag(Gtk::ListItem& listItem)
    {
      if (closing)
      {
        return {};
      }

      auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(listItem.get_item());

      if (rowPtr == nullptr)
      {
        return {};
      }

      auto selected = selectionController.selectedTrackIds();

      if (!std::ranges::contains(selected, rowPtr->trackId()))
      {
        selectionController.selectTrack(rowPtr->trackId());
        selected = selectionController.selectedTrackIds();
      }

      auto sessionRes =
        uimodel::ListOrderAuthoringSession::begin(runtime.library(), runtime.views(), viewId, textCatalog);

      if (!sessionRes)
      {
        showStatus(sessionRes.error().message);
        return {};
      }

      clearActiveDrag();
      sessionPtr = std::move(*sessionRes);
      selectedTrackIds = uimodel::listOrderDragSelection(rowPtr->trackId(), selected, sessionPtr->effectiveTrackIds());

      if (selectedTrackIds.empty())
      {
        clearActiveDrag();
        return {};
      }

      token = std::format("aobus-list-order:{}:{}", viewId.raw(), nextDragToken());
      invalidatedSubscription = sessionPtr->onInvalidated(
        [weakStatePtr = std::weak_ptr<State>{shared_from_this()}] noexcept
        {
          auto statePtr = weakStatePtr.lock();

          if (statePtr == nullptr || statePtr->closing)
          {
            return;
          }

          statePtr->invalidated = true;
          statePtr->token.clear();
          statePtr->clearIndicator();
          statePtr->showStatus(statePtr->sessionPtr != nullptr
                                 ? statePtr->sessionPtr->capabilities().disabledReason
                                 : gtkText(statePtr->textCatalog, i18n::MessageId::ListOrderChanged));
        });
      clearStatus();
      return stringContentProvider(token);
    }

    std::optional<std::size_t> gapIndex(Gtk::ListItem const& listItem,
                                        Gtk::Widget const& cell,
                                        double const yPosition) const
    {
      if (auto const position = listItem.get_position(); position != kInvalidListPosition)
      {
        auto const after = yPosition >= static_cast<double>(std::max(1, cell.get_height())) / 2.0;
        return static_cast<std::size_t>(position) + (after ? 1U : 0U);
      }

      return std::nullopt;
    }

    void updateIndicator(Gtk::ListItem const& /*listItem*/, Gtk::Widget& cell, double const yPosition)
    {
      if (closing || sessionPtr == nullptr || invalidated)
      {
        clearIndicator();
        return;
      }

      auto* const row = rowWidgetFor(cell);

      if (row == nullptr)
      {
        clearIndicator();
        return;
      }

      if (indicatorRow != row)
      {
        clearIndicator();
        indicatorRow = row;
      }

      auto const after = yPosition >= static_cast<double>(std::max(1, cell.get_height())) / 2.0;
      row->remove_css_class(after ? kDropBeforeCssClass : kDropAfterCssClass);
      row->add_css_class(after ? kDropAfterCssClass : kDropBeforeCssClass);
      autoScroll(cell, yPosition);
    }

    void autoScroll(Gtk::Widget& cell, double const yPosition)
    {
      auto const optPoint =
        cell.compute_point(scrolledWindow, Gdk::Graphene::Point{0.0F, static_cast<float>(yPosition)});

      if (!optPoint)
      {
        return;
      }

      auto const adjustmentPtr = scrolledWindow.get_vadjustment();

      if (!adjustmentPtr)
      {
        return;
      }

      double delta = 0.0;
      double const scrollYPosition = static_cast<double>(optPoint->get_y());
      auto const height = static_cast<double>(scrolledWindow.get_height());

      if (scrollYPosition < kAutoScrollEdge)
      {
        delta = -kAutoScrollStep;
      }
      else if (scrollYPosition > height - kAutoScrollEdge)
      {
        delta = kAutoScrollStep;
      }

      if (delta == 0.0)
      {
        return;
      }

      auto const maximum =
        std::max(adjustmentPtr->get_lower(), adjustmentPtr->get_upper() - adjustmentPtr->get_page_size());
      adjustmentPtr->set_value(std::clamp(adjustmentPtr->get_value() + delta, adjustmentPtr->get_lower(), maximum));
    }

    bool drop(Gtk::ListItem const& listItem,
              Gtk::Widget const& cell,
              Glib::ValueBase const& value,
              double const yPosition)
    {
      if (auto const optToken = stringFromDropValue(value);
          closing || invalidated || sessionPtr == nullptr || !optToken || *optToken != token)
      {
        clearIndicator();
        return false;
      }

      auto const optGapIndex = gapIndex(listItem, cell, yPosition);

      if (!optGapIndex)
      {
        clearIndicator();
        return false;
      }

      auto const anchorRes =
        uimodel::listOrderAnchorForGap(sessionPtr->effectiveTrackIds(), selectedTrackIds, *optGapIndex);

      if (!anchorRes)
      {
        showStatus(anchorRes.error().message);
        clearActiveDrag();
        return false;
      }

      auto dropSessionPtr = std::move(sessionPtr);
      auto selectedIds = std::move(selectedTrackIds);
      auto submission = dropSessionPtr->moveBefore(std::move(selectedIds), *anchorRes);
      clearActiveDrag();
      spawnUiTask(runtime.async(),
                  tasks,
                  *this,
                  "track order drop",
                  std::move(submission),
                  [](State* state, auto result)
                  {
                    if (!result)
                    {
                      APP_LOG_ERROR("Track order drop failed: {}", result.error().message);
                      state->showStatus(result.error().message);
                      return;
                    }

                    switch (result->status)
                    {
                      case rt::AuthoringStatus::Applied:
                        state->showStatus(i18n::requiredFormat(state->textCatalog,
                                                               i18n::MessageId::ListOrderMoved,
                                                               {{"count", result->reply.selectedTrackIds.size()}}));
                        break;
                      case rt::AuthoringStatus::NoOp:
                        state->showStatus(gtkText(state->textCatalog, i18n::MessageId::ListOrderUnchanged));
                        break;
                      case rt::AuthoringStatus::Busy:
                        state->showStatus(gtkText(state->textCatalog, i18n::MessageId::ListOrderLibraryBusy));
                        break;
                      case rt::AuthoringStatus::Stale:
                        state->showStatus(gtkText(state->textCatalog, i18n::MessageId::ListOrderChanged));
                        break;
                      case rt::AuthoringStatus::Unavailable:
                        state->showStatus(gtkText(state->textCatalog, i18n::MessageId::ListOrderEditingUnavailable));
                        break;
                    }
                  });
      return true;
    }

    rt::AppRuntime& runtime;
    rt::ViewId viewId = rt::kInvalidViewId;
    i18n::MessageCatalog textCatalog;
    Gtk::ScrolledWindow& scrolledWindow;
    TrackSelectionController& selectionController;
    Callbacks callbacks;
    std::unique_ptr<uimodel::ListOrderAuthoringSession> sessionPtr{};
    std::vector<TrackId> selectedTrackIds{};
    std::string token;
    async::Subscription invalidatedSubscription;
    Gtk::Widget* indicatorRow = nullptr;
    bool invalidated = false;
    bool closing = false;
    async::LifetimeScope tasks;
  };

  TrackOrderDragController::TrackOrderDragController(rt::AppRuntime& runtime,
                                                     rt::ViewId const viewId,
                                                     i18n::MessageCatalog const& textCatalog,
                                                     Gtk::ColumnView& /*columnView*/,
                                                     Gtk::ScrolledWindow& scrolledWindow,
                                                     TrackSelectionController& selectionController,
                                                     Callbacks callbacks)
    : _statePtr{std::make_shared<State>(runtime,
                                        viewId,
                                        textCatalog,
                                        scrolledWindow,
                                        selectionController,
                                        std::move(callbacks))}
    , _columnPtr{makeColumn(_statePtr)}
  {
  }

  Glib::RefPtr<Gtk::ColumnViewColumn> TrackOrderDragController::makeColumn(std::shared_ptr<State> const& statePtr)
  {
    auto const factoryPtr = Gtk::SignalListItemFactory::create();

    factoryPtr->signal_setup().connect(
      [statePtr](Glib::RefPtr<Gtk::ListItem> const& listItemPtr)
      {
        auto* const cell = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        cell->set_halign(Gtk::Align::CENTER);
        cell->set_valign(Gtk::Align::FILL);
        cell->set_hexpand(true);
        cell->add_css_class("ao-order-drag-handle");
        cell->set_cursor("grab");
        cell->set_tooltip_text(gtkText(statePtr->textCatalog, i18n::MessageId::GtkManualOrderDrag));
        setAccessibleLabel(*cell, gtkText(statePtr->textCatalog, i18n::MessageId::GtkManualOrderRearrange));

        auto* const image = Gtk::make_managed<Gtk::Image>();
        image->set_from_icon_name("list-drag-handle-symbolic");
        image->set_pixel_size(16);
        image->set_halign(Gtk::Align::CENTER);
        image->set_valign(Gtk::Align::CENTER);
        cell->append(*image);

        auto const dragSourcePtr = Gtk::DragSource::create();
        dragSourcePtr->set_actions(Gdk::DragAction::MOVE);
        dragSourcePtr->signal_prepare().connect([statePtr, listItemRaw = listItemPtr.get()](double, double)
                                                { return statePtr->beginDrag(*listItemRaw); },
                                                false);
        dragSourcePtr->signal_drag_end().connect([statePtr](Glib::RefPtr<Gdk::Drag> const&, bool)
                                                 { statePtr->clearActiveDrag(); });
        cell->add_controller(dragSourcePtr);

        auto const dropTargetPtr =
          Gtk::DropTarget::create(Glib::Value<std::string>::value_type(), Gdk::DragAction::MOVE);
        dropTargetPtr->signal_motion().connect(
          [statePtr, listItemRaw = listItemPtr.get(), cell](double, double yPosition)
          {
            statePtr->updateIndicator(*listItemRaw, *cell, yPosition);
            return statePtr->sessionPtr != nullptr && !statePtr->invalidated ? Gdk::DragAction::MOVE
                                                                             : Gdk::DragAction::NONE;
          },
          false);
        dropTargetPtr->signal_leave().connect([statePtr] { statePtr->clearIndicator(); });
        dropTargetPtr->signal_drop().connect(
          [statePtr, listItemRaw = listItemPtr.get(), cell](Glib::ValueBase const& value, double, double yPosition)
          { return statePtr->drop(*listItemRaw, *cell, value, yPosition); },
          false);
        cell->add_controller(dropTargetPtr);
        listItemPtr->set_child(*cell);
      });

    factoryPtr->signal_bind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItemPtr)
      {
        auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(listItemPtr->get_item());

        if (auto* const child = listItemPtr->get_child(); child != nullptr && rowPtr != nullptr)
        {
          ::g_object_set_data(G_OBJECT(child->gobj()),
                              kBoundTrackIdDataKey,
                              GUINT_TO_POINTER(static_cast<guint>(rowPtr->trackId().raw())));
        }
      });

    factoryPtr->signal_unbind().connect(
      [statePtr](Glib::RefPtr<Gtk::ListItem> const& listItemPtr)
      {
        if (auto* const child = listItemPtr->get_child(); child != nullptr)
        {
          if (rowWidgetFor(*child) == statePtr->indicatorRow)
          {
            statePtr->clearIndicator();
          }

          ::g_object_set_data(G_OBJECT(child->gobj()), kBoundTrackIdDataKey, nullptr);
        }
      });

    auto columnPtr = Gtk::ColumnViewColumn::create("", factoryPtr);
    columnPtr->set_id("list-order-handle");
    columnPtr->set_fixed_width(kDragHandleColumnWidth);
    columnPtr->set_resizable(false);
    return columnPtr;
  }

  TrackOrderDragController::~TrackOrderDragController()
  {
    _statePtr->close();
  }
} // namespace ao::gtk

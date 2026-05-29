// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackSelectionController.h"

#include "track/TrackFieldUi.h"
#include "track/TrackListModel.h"
#include "track/TrackRowObject.h"
#include <ao/Type.h>

#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <glib.h>
#include <glibmm/refptr.h>
#include <gtkmm/columnview.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gesture.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>
#include <sigc++/functors/mem_fun.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    bool isTagsCellWidget(Gtk::Widget const* widget)
    {
      for (auto const* current = widget; current != nullptr; current = current->get_parent())
      {
        if (current->has_css_class(detail::kTagsCellCssClass))
        {
          return true;
        }
      }

      return false;
    }

    Gtk::Stack* findInlineEditStack(Gtk::Widget const* widget)
    {
      auto* current = const_cast<Gtk::Widget*>(widget); // NOLINT(cppcoreguidelines-pro-type-const-cast)

      while (current != nullptr)
      {
        if (auto* const stack = dynamic_cast<Gtk::Stack*>(current);
            stack != nullptr && stack->get_child_by_name("edit") != nullptr)
        {
          return stack;
        }

        current = current->get_parent();
      }

      return nullptr;
    }
  } // namespace

  TrackSelectionController::TrackSelectionController(Gtk::ColumnView& columnView,
                                                     Glib::RefPtr<TrackListModel> modelPtr,
                                                     Glib::RefPtr<Gtk::MultiSelection> selectionModelPtr)
    : _columnView{columnView}
    , _modelPtr{std::move(modelPtr)}
    , _selectionModelPtr{std::move(selectionModelPtr)}
    , _selectionChangedConnection{_selectionModelPtr->signal_selection_changed().connect(
        sigc::mem_fun(*this, &TrackSelectionController::onSelectionChanged))}
  {
  }

  void TrackSelectionController::setupActivation()
  {
    _columnView.set_focusable(true);
    _columnView.set_focus_on_click(true);

    _columnView.signal_activate().connect(
      [this](std::uint32_t position)
      {
        if (_suppressNextTrackActivation)
        {
          _suppressNextTrackActivation = false;
          return;
        }

        if (auto const trackId = trackIdAtPosition(position); trackId != kInvalidTrackId)
        {
          _trackActivated.emit(trackId);
          return;
        }

        onActivateCurrentSelection();
      });

    auto const keyControllerPtr = Gtk::EventControllerKey::create();
    keyControllerPtr->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType modifiers)
      {
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
        {
          onActivateCurrentSelection();
          return true;
        }

        if (keyval == GDK_KEY_t || keyval == GDK_KEY_T)
        {
          if (static_cast<bool>(modifiers & Gdk::ModifierType::CONTROL_MASK))
          {
            if (auto const selectedIds = selectedTrackIds(); !selectedIds.empty())
            {
              _tagEditRequested.emit(selectedIds, nullptr);
            }

            return true;
          }
        }

        return false;
      },
      false);

    _columnView.add_controller(keyControllerPtr);

    auto const primaryClickControllerPtr = Gtk::GestureClick::create();
    primaryClickControllerPtr->set_button(GDK_BUTTON_PRIMARY);
    primaryClickControllerPtr->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);

    primaryClickControllerPtr->signal_pressed().connect(
      [this, primaryClickControllerPtr](std::int32_t nPress, double xPos, double yPos)
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

        auto const selectedIds = selectedTrackIds();

        if (selectedIds.empty())
        {
          return;
        }

        primaryClickControllerPtr->set_state(Gtk::EventSequenceState::CLAIMED);
        _suppressNextTrackActivation = true;
        _tagEditRequested.emit(selectedIds, dynamic_cast<Gtk::Widget*>(target));
      });

    _columnView.add_controller(primaryClickControllerPtr);

    auto const longPressControllerPtr = Gtk::GestureLongPress::create();
    longPressControllerPtr->set_touch_only(false);
    longPressControllerPtr->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);

    longPressControllerPtr->signal_pressed().connect(
      [this, longPressControllerPtr](double xPos, double yPos)
      {
        auto* const target = _columnView.pick(xPos, yPos, Gtk::PickFlags::NON_TARGETABLE);
        auto* const stack = findInlineEditStack(target);

        if (stack == nullptr)
        {
          return;
        }

        longPressControllerPtr->set_state(Gtk::EventSequenceState::CLAIMED);
        _suppressNextTrackActivation = true;
        stack->set_visible_child("edit");

        if (auto* const entry = dynamic_cast<Gtk::Entry*>(stack->get_child_by_name("edit")); entry != nullptr)
        {
          entry->grab_focus();
        }
      });

    _columnView.add_controller(longPressControllerPtr);

    auto const secondaryClickControllerPtr = Gtk::GestureClick::create();
    secondaryClickControllerPtr->set_button(GDK_BUTTON_SECONDARY);

    secondaryClickControllerPtr->signal_released().connect(
      [this](std::int32_t, double xPos, double yPos)
      {
        if (selectedTrackCount() == 0)
        {
          return;
        }

        _contextMenuRequested.emit(xPos, yPos);
      });

    _columnView.add_controller(secondaryClickControllerPtr);
  }

  void TrackSelectionController::onActivateCurrentSelection()
  {
    if (_suppressNextTrackActivation)
    {
      _suppressNextTrackActivation = false;
      return;
    }

    if (auto const trackId = primarySelectedTrackId(); trackId != kInvalidTrackId)
    {
      _trackActivated.emit(trackId);
    }
  }

  void TrackSelectionController::onSelectionChanged(std::uint32_t /*position*/, std::uint32_t /*nItems*/)
  {
    _selectionChanged.emit();
  }

  TrackId TrackSelectionController::trackIdAtPosition(std::uint32_t position) const noexcept
  {
    if (!_selectionModelPtr)
    {
      return kInvalidTrackId;
    }

    auto const itemPtr = _selectionModelPtr->get_object(position);

    if (!itemPtr)
    {
      return kInvalidTrackId;
    }

    auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(itemPtr);

    if (!rowPtr)
    {
      return kInvalidTrackId;
    }

    return rowPtr->trackId();
  }

  std::size_t TrackSelectionController::selectedTrackCount() const noexcept
  {
    if (auto const bitsetPtr = _selectionModelPtr->get_selection(); bitsetPtr)
    {
      return bitsetPtr->get_size();
    }

    return 0;
  }

  std::vector<TrackId> TrackSelectionController::selectedTrackIds() const noexcept
  {
    auto const modelPtr = _selectionModelPtr->get_model();

    if (!modelPtr)
    {
      return {};
    }

    return std::views::iota(0U, modelPtr->get_n_items()) |
           std::views::filter([this](auto idx) { return _selectionModelPtr->is_selected(idx); }) |
           std::views::transform([this](auto idx) { return trackIdAtPosition(idx); }) |
           std::views::filter([](auto const& id) { return id != kInvalidTrackId; }) | std::ranges::to<std::vector>();
  }

  std::vector<Glib::RefPtr<TrackRowObject>> TrackSelectionController::selectedRows() const noexcept
  {
    auto const modelPtr = _selectionModelPtr->get_model();

    if (!modelPtr)
    {
      return {};
    }

    return std::views::iota(0U, modelPtr->get_n_items()) |
           std::views::filter([this](auto idx) { return _selectionModelPtr->is_selected(idx); }) |
           std::views::transform([modelPtr](auto idx)
                                 { return std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(idx)); }) |
           std::views::filter([](auto const& row) { return static_cast<bool>(row); }) | std::ranges::to<std::vector>();
  }

  std::chrono::milliseconds TrackSelectionController::selectedTracksDuration() const noexcept
  {
    auto const modelPtr = _selectionModelPtr->get_model();

    if (!modelPtr)
    {
      return std::chrono::milliseconds{0};
    }

    return std::ranges::fold_left(
      std::views::iota(0U, modelPtr->get_n_items()) |
        std::views::filter([this](auto idx) { return _selectionModelPtr->is_selected(idx); }) |
        std::views::transform([modelPtr](auto idx)
                              { return std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(idx)); }) |
        std::views::filter([](auto const& row) { return static_cast<bool>(row); }) |
        std::views::transform([](auto const& row) { return row->duration(); }),
      std::chrono::milliseconds{0},
      std::plus<>{});
  }

  TrackId TrackSelectionController::primarySelectedTrackId() const noexcept
  {
    auto const bitsetPtr = _selectionModelPtr->get_selection();

    if (!bitsetPtr || bitsetPtr->get_size() == 0)
    {
      return kInvalidTrackId;
    }

    return trackIdAtPosition(static_cast<std::uint32_t>(bitsetPtr->get_nth(0)));
  }

  void TrackSelectionController::selectTrack(TrackId trackId)
  {
    auto const optIndex = _modelPtr->indexOf(trackId);

    if (!optIndex || *optIndex >= _selectionModelPtr->get_n_items())
    {
      return;
    }

    auto const pos = static_cast<guint>(*optIndex);

    _selectionModelPtr->select_item(pos, true);
    _columnView.scroll_to(pos, nullptr, Gtk::ListScrollFlags::FOCUS | Gtk::ListScrollFlags::SELECT, nullptr);
  }

  void TrackSelectionController::scrollToTrack(TrackId trackId)
  {
    auto const optIndex = _modelPtr->indexOf(trackId);

    if (!optIndex || *optIndex >= _selectionModelPtr->get_n_items())
    {
      return;
    }

    auto const pos = static_cast<guint>(*optIndex);
    _columnView.scroll_to(pos, nullptr, Gtk::ListScrollFlags::NONE, {});
  }

  void TrackSelectionController::setPlayingTrackId(TrackId trackId)
  {
    _playingTrackId = trackId;
    _modelPtr->setPlayingTrackId(trackId);
  }

  std::vector<TrackId> TrackSelectionController::visibleTrackIds() const noexcept
  {
    auto* const proj = _modelPtr->projection();

    if (proj == nullptr)
    {
      return {};
    }

    return std::views::iota(0UZ, proj->size()) |
           std::views::transform([proj](auto idx) { return proj->trackIdAt(idx); }) | std::ranges::to<std::vector>();
  }
} // namespace ao::gtk

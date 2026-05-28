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
                                                     Glib::RefPtr<TrackListModel> model,
                                                     Glib::RefPtr<Gtk::MultiSelection> selectionModel)
    : _columnView{columnView}
    , _model{std::move(model)}
    , _selectionModel{std::move(selectionModel)}
    , _selectionChangedConnection{_selectionModel->signal_selection_changed().connect(
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

    auto const keyController = Gtk::EventControllerKey::create();
    keyController->signal_key_pressed().connect(
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

    _columnView.add_controller(keyController);

    auto const primaryClickController = Gtk::GestureClick::create();
    primaryClickController->set_button(GDK_BUTTON_PRIMARY);
    primaryClickController->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);

    primaryClickController->signal_pressed().connect(
      [this, primaryClickController](std::int32_t nPress, double xPos, double yPos)
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

        primaryClickController->set_state(Gtk::EventSequenceState::CLAIMED);
        _suppressNextTrackActivation = true;
        _tagEditRequested.emit(selectedIds, dynamic_cast<Gtk::Widget*>(target));
      });

    _columnView.add_controller(primaryClickController);

    auto const longPressController = Gtk::GestureLongPress::create();
    longPressController->set_touch_only(false);
    longPressController->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);

    longPressController->signal_pressed().connect(
      [this, longPressController](double xPos, double yPos)
      {
        auto* const target = _columnView.pick(xPos, yPos, Gtk::PickFlags::NON_TARGETABLE);
        auto* const stack = findInlineEditStack(target);

        if (stack == nullptr)
        {
          return;
        }

        longPressController->set_state(Gtk::EventSequenceState::CLAIMED);
        _suppressNextTrackActivation = true;
        stack->set_visible_child("edit");

        if (auto* const entry = dynamic_cast<Gtk::Entry*>(stack->get_child_by_name("edit")))
        {
          entry->grab_focus();
        }
      });

    _columnView.add_controller(longPressController);

    auto const secondaryClickController = Gtk::GestureClick::create();
    secondaryClickController->set_button(GDK_BUTTON_SECONDARY);

    secondaryClickController->signal_released().connect(
      [this](std::int32_t, double xPos, double yPos)
      {
        if (selectedTrackCount() == 0)
        {
          return;
        }

        _contextMenuRequested.emit(xPos, yPos);
      });

    _columnView.add_controller(secondaryClickController);
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
    if (!_selectionModel)
    {
      return kInvalidTrackId;
    }

    auto const item = _selectionModel->get_object(position);

    if (!item)
    {
      return kInvalidTrackId;
    }

    auto const row = std::dynamic_pointer_cast<TrackRowObject>(item);

    if (!row)
    {
      return kInvalidTrackId;
    }

    return row->trackId();
  }

  std::size_t TrackSelectionController::selectedTrackCount() const noexcept
  {
    if (auto const bitset = _selectionModel->get_selection())
    {
      return bitset->get_size();
    }

    return 0;
  }

  std::vector<TrackId> TrackSelectionController::selectedTrackIds() const noexcept
  {
    auto const model = _selectionModel->get_model();

    if (!model)
    {
      return {};
    }

    return std::views::iota(0U, model->get_n_items()) |
           std::views::filter([this](auto idx) { return _selectionModel->is_selected(idx); }) |
           std::views::transform([this](auto idx) { return trackIdAtPosition(idx); }) |
           std::views::filter([](auto const& id) { return id != kInvalidTrackId; }) | std::ranges::to<std::vector>();
  }

  std::vector<Glib::RefPtr<TrackRowObject>> TrackSelectionController::selectedRows() const noexcept
  {
    auto const model = _selectionModel->get_model();

    if (!model)
    {
      return {};
    }

    return std::views::iota(0U, model->get_n_items()) |
           std::views::filter([this](auto idx) { return _selectionModel->is_selected(idx); }) |
           std::views::transform([model](auto idx)
                                 { return std::dynamic_pointer_cast<TrackRowObject>(model->get_object(idx)); }) |
           std::views::filter([](auto const& row) { return static_cast<bool>(row); }) | std::ranges::to<std::vector>();
  }

  std::chrono::milliseconds TrackSelectionController::selectedTracksDuration() const noexcept
  {
    auto const model = _selectionModel->get_model();

    if (!model)
    {
      return std::chrono::milliseconds{0};
    }

    return std::ranges::fold_left(
      std::views::iota(0U, model->get_n_items()) |
        std::views::filter([this](auto idx) { return _selectionModel->is_selected(idx); }) |
        std::views::transform([model](auto idx)
                              { return std::dynamic_pointer_cast<TrackRowObject>(model->get_object(idx)); }) |
        std::views::filter([](auto const& row) { return static_cast<bool>(row); }) |
        std::views::transform([](auto const& row) { return row->duration(); }),
      std::chrono::milliseconds{0},
      std::plus<>{});
  }

  TrackId TrackSelectionController::primarySelectedTrackId() const noexcept
  {
    auto const bitset = _selectionModel->get_selection();

    if (!bitset || bitset->get_size() == 0)
    {
      return kInvalidTrackId;
    }

    return trackIdAtPosition(static_cast<std::uint32_t>(bitset->get_nth(0)));
  }

  void TrackSelectionController::selectTrack(TrackId trackId)
  {
    auto const optIndex = _model->indexOf(trackId);

    if (!optIndex || *optIndex >= _selectionModel->get_n_items())
    {
      return;
    }

    auto const pos = static_cast<guint>(*optIndex);

    _selectionModel->select_item(pos, true);
    _columnView.scroll_to(pos, nullptr, Gtk::ListScrollFlags::FOCUS | Gtk::ListScrollFlags::SELECT, nullptr);
  }

  void TrackSelectionController::scrollToTrack(TrackId trackId)
  {
    auto const optIndex = _model->indexOf(trackId);

    if (!optIndex || *optIndex >= _selectionModel->get_n_items())
    {
      return;
    }

    auto const pos = static_cast<guint>(*optIndex);
    _columnView.scroll_to(pos, nullptr, Gtk::ListScrollFlags::NONE, {});
  }

  void TrackSelectionController::setPlayingTrackId(TrackId trackId)
  {
    _playingTrackId = trackId;
    _model->setPlayingTrackId(trackId);
  }

  std::vector<TrackId> TrackSelectionController::visibleTrackIds() const noexcept
  {
    auto* const proj = _model->projection();

    if (proj == nullptr)
    {
      return {};
    }

    return std::views::iota(0UZ, proj->size()) |
           std::views::transform([proj](auto idx) { return proj->trackIdAt(idx); }) | std::ranges::to<std::vector>();
  }
} // namespace ao::gtk

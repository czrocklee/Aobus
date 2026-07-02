// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackSelectionController.h"

#include "track/TrackFieldUi.h"
#include "track/TrackListModel.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>

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
#include <gtkmm/selectionmodel.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>
#include <sigc++/functors/mem_fun.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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
        if (current->has_css_class(kTagsCellCssClass))
        {
          return true;
        }
      }

      return false;
    }

    Gtk::Stack* findInlineEditStack(Gtk::Widget* widget)
    {
      auto* current = widget;

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

    std::vector<std::uint32_t> selectedPositions(Gtk::SelectionModel const& selectionModel)
    {
      auto const bitsetPtr = selectionModel.get_selection();

      if (!bitsetPtr)
      {
        return {};
      }

      auto const selectedCount = bitsetPtr->get_size();
      auto positions = std::vector<std::uint32_t>{};
      positions.reserve(selectedCount);

      for (std::uint32_t idx = 0U; idx < selectedCount; ++idx)
      {
        positions.push_back(static_cast<std::uint32_t>(bitsetPtr->get_nth(idx)));
      }

      return positions;
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
    if (!_selectionModelPtr->get_model())
    {
      return {};
    }

    auto ids = std::vector<TrackId>{};

    for (auto const position : selectedPositions(*_selectionModelPtr))
    {
      if (auto const trackId = trackIdAtPosition(position); trackId != kInvalidTrackId)
      {
        ids.push_back(trackId);
      }
    }

    return ids;
  }

  std::vector<Glib::RefPtr<TrackRowObject>> TrackSelectionController::selectedRows() const noexcept
  {
    auto const modelPtr = _selectionModelPtr->get_model();

    if (!modelPtr)
    {
      return {};
    }

    auto rows = std::vector<Glib::RefPtr<TrackRowObject>>{};

    for (auto const position : selectedPositions(*_selectionModelPtr))
    {
      if (auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(position)); rowPtr)
      {
        rows.push_back(rowPtr);
      }
    }

    return rows;
  }

  std::chrono::milliseconds TrackSelectionController::selectedTracksDuration() const noexcept
  {
    auto const modelPtr = _selectionModelPtr->get_model();

    if (!modelPtr)
    {
      return std::chrono::milliseconds{0};
    }

    auto totalDuration = std::chrono::milliseconds{0};

    for (auto const position : selectedPositions(*_selectionModelPtr))
    {
      if (auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(position)); rowPtr)
      {
        totalDuration += rowPtr->duration();
      }
    }

    return totalDuration;
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

    // Always try to scroll the group header into view for context
    auto scrollPos = pos;

    if (auto const optGroupIdx = _modelPtr->groupIndexForTrack(trackId); optGroupIdx)
    {
      if (auto* const proj = _modelPtr->projection(); proj != nullptr)
      {
        scrollPos = static_cast<guint>(proj->groupAt(*optGroupIdx).rows.start);
      }
    }

    _columnView.scroll_to(scrollPos, nullptr, Gtk::ListScrollFlags::FOCUS | Gtk::ListScrollFlags::SELECT, nullptr);
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

    auto ids = std::vector<TrackId>{};
    ids.reserve(proj->size());

    for (std::size_t idx = 0; idx < proj->size(); ++idx)
    {
      ids.push_back(proj->trackIdAt(idx));
    }

    return ids;
  }
} // namespace ao::gtk

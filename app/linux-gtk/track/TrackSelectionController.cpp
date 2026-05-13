// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackSelectionController.h"

#include "shell/ThemeBus.h"
#include <ao/utility/Log.h>

#include <gdk/gdk.h>
#include <gtkmm/columnview.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>

#include <cstdint>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    bool isTagsCellWidget(Gtk::Widget const* widget)
    {
      for (auto const* current = widget; current != nullptr; current = current->get_parent())
      {
        if (current->has_css_class("track-tags-cell"))
        {
          return true;
        }
      }

      return false;
    }
  }

  TrackSelectionController::TrackSelectionController(Gtk::ColumnView& columnView,
                                                     TrackListAdapter& adapter,
                                                     Glib::RefPtr<Gtk::MultiSelection> selectionModel)
    : _columnView{columnView}
    , _adapter{adapter}
    , _selectionModel{std::move(selectionModel)}
  {
    _selectionChangedConnection = _selectionModel->signal_selection_changed().connect(
      sigc::mem_fun(*this, &TrackSelectionController::onSelectionChanged));
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

        if (auto const trackId = trackIdAtPosition(position))
        {
          _trackActivated.emit(*trackId);
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
      [this](int, double xPos, double yPos)
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

    if (auto const trackId = getPrimarySelectedTrackId(); trackId)
    {
      _trackActivated.emit(*trackId);
    }
  }

  void TrackSelectionController::onSelectionChanged(std::uint32_t /*position*/, std::uint32_t /*nItems*/)
  {
    _selectionChanged.emit();
  }

  std::optional<TrackSelectionController::TrackId> TrackSelectionController::trackIdAtPosition(
    std::uint32_t position) const
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

    auto const row = std::dynamic_pointer_cast<TrackRowObject>(item);

    if (!row)
    {
      return std::nullopt;
    }

    return row->getTrackId();
  }

  std::size_t TrackSelectionController::selectedTrackCount() const
  {
    if (auto const bitset = _selectionModel->get_selection())
    {
      return bitset->get_size();
    }

    return 0;
  }

  std::vector<TrackSelectionController::TrackId> TrackSelectionController::getSelectedTrackIds() const
  {
    auto result = std::vector<TrackId>{};
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
        if (auto const trackId = trackIdAtPosition(i))
        {
          result.push_back(*trackId);
        }
      }
    }

    return result;
  }

  std::vector<Glib::RefPtr<TrackRowObject>> TrackSelectionController::getSelectedRows() const
  {
    auto result = std::vector<Glib::RefPtr<TrackRowObject>>{};
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

        if (auto row = std::dynamic_pointer_cast<TrackRowObject>(item))
        {
          result.push_back(std::move(row));
        }
      }
    }

    return result;
  }

  std::chrono::milliseconds TrackSelectionController::getSelectedTracksDuration() const
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

        if (auto const row = std::dynamic_pointer_cast<TrackRowObject>(item))
        {
          totalDuration += row->getDuration();
        }
      }
    }

    return totalDuration;
  }

  std::optional<TrackSelectionController::TrackId> TrackSelectionController::getPrimarySelectedTrackId() const
  {
    auto const bitset = _selectionModel->get_selection();

    if (!bitset || bitset->get_size() == 0)
    {
      return std::nullopt;
    }

    return trackIdAtPosition(static_cast<std::uint32_t>(bitset->get_nth(0)));
  }

  void TrackSelectionController::selectTrack(TrackId trackId)
  {
    auto const optIndex = _adapter.indexOf(trackId);

    if (!optIndex || *optIndex >= _selectionModel->get_n_items())
    {
      return;
    }

    auto const pos = static_cast<guint>(*optIndex);

    _selectionModel->select_item(pos, true);
    _columnView.scroll_to(pos, nullptr, Gtk::ListScrollFlags::FOCUS | Gtk::ListScrollFlags::SELECT, nullptr);
  }

  void TrackSelectionController::setPlayingTrackId(std::optional<TrackId> trackId)
  {
    auto const model = _selectionModel->get_model();

    if (!model)
    {
      return;
    }

    if (_playingTrackId)
    {
      if (auto const optIdx = _adapter.indexOf(*_playingTrackId); optIdx && *optIdx < model->get_n_items())
      {
        auto const item = model->get_object(static_cast<guint>(*optIdx));

        if (auto const row = std::dynamic_pointer_cast<TrackRowObject>(item))
        {
          row->setPlaying(false);
        }
      }
    }

    _playingTrackId = trackId;

    if (trackId)
    {
      if (auto const optIdx = _adapter.indexOf(*trackId); optIdx && *optIdx < model->get_n_items())
      {
        auto const item = model->get_object(static_cast<guint>(*optIdx));

        if (auto const row = std::dynamic_pointer_cast<TrackRowObject>(item))
        {
          row->setPlaying(true);
        }
      }
    }
  }

  std::vector<TrackSelectionController::TrackId> TrackSelectionController::getVisibleTrackIds() const
  {
    auto result = std::vector<TrackId>{};
    auto* const proj = _adapter.projection();

    if (proj == nullptr)
    {
      return result;
    }

    result.reserve(proj->size());

    for (std::size_t i = 0; i < proj->size(); ++i)
    {
      result.push_back(proj->trackIdAt(i));
    }

    return result;
  }
} // namespace ao::gtk

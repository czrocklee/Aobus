// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingFieldLabel.h"

#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <gdkmm/cursor.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>
#include <pangomm/layout.h>
#include <unistd.h>

#include <cstdint>

namespace ao::gtk
{
  namespace
  {
    char const* cssClassForField(rt::TrackField field)
    {
      switch (field)
      {
        case rt::TrackField::Title: return "ao-playback-title";
        case rt::TrackField::Artist: return "ao-playback-artist";
        default: return "";
      }
    }
  } // namespace

  NowPlayingFieldLabel::NowPlayingFieldLabel(rt::AppRuntime& runtime, rt::TrackField field, Action action)
    : _runtime{runtime}
    , _field{field}
    , _action{action}
    , _controller{_runtime.playback(),
                  [this](ao::uimodel::playback::NowPlayingViewState const& view) { applyState(view); }}
  {
    _label.set_ellipsize(Pango::EllipsizeMode::END);

    if (auto const* cssClass = cssClassForField(field); cssClass[0] != '\0')
    {
      _label.add_css_class(cssClass);
    }

    _label.set_selectable(false);

    if (_action != Action::None)
    {
      _label.add_css_class("ao-clickable");
      _label.set_cursor(Gdk::Cursor::create("pointer"));

      auto const gesturePtr = Gtk::GestureClick::create();
      gesturePtr->set_button(1);
      gesturePtr->set_propagation_phase(Gtk::PropagationPhase::BUBBLE);
      gesturePtr->signal_pressed().connect([this](std::int32_t, double, double) { onLabelClicked(); });
      _label.add_controller(gesturePtr);
    }
  }

  void NowPlayingFieldLabel::onLabelClicked()
  {
    APP_LOG_DEBUG("[PID {}] NowPlayingFieldLabel: Clicked, field: {}, action: {}",
                  getpid(),
                  static_cast<int>(_field),
                  static_cast<int>(_action));

    auto const cmd = _controller.resolveAction(_action, _field);

    using Type = uimodel::playback::NowPlayingActionCommand::Type;

    switch (cmd.type)
    {
      case Type::Reveal: _runtime.playback().revealPlayingTrack(); break;

      case Type::Pause: _runtime.playback().pause(); break;

      case Type::Resume: _runtime.playback().resume(); break;

      case Type::Navigate:
        APP_LOG_DEBUG("[PID {}] NowPlayingFieldLabel: Navigating to query: {}", getpid(), cmd.navigateQuery);
        _runtime.workspace().navigateTo(
          rt::FilteredListTarget{.listId = rt::kAllTracksListId, .filterExpression = cmd.navigateQuery});
        break;

      case Type::None:
      default: break;
    }
  }

  void NowPlayingFieldLabel::applyState(ao::uimodel::playback::NowPlayingViewState const& view)
  {
    _label.set_text(uimodel::playback::NowPlayingViewModel::fieldText(view, _field));
  }
} // namespace ao::gtk

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingFieldLabel.h"

#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <gdkmm/cursor.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>
#include <pangomm/layout.h>
#include <unistd.h>

#include <cstdint>
#include <tuple>

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
    , _nowPlayingViewModel{_runtime.playback(),
                           [this](ao::uimodel::NowPlayingViewState const& view) { applyState(view); }}
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
      gesturePtr->signal_pressed().connect([this](std::int32_t, double, double) { handleLabelClicked(); });
      _label.add_controller(gesturePtr);
    }
  }

  void NowPlayingFieldLabel::handleLabelClicked()
  {
    APP_LOG_DEBUG("[PID {}] NowPlayingFieldLabel: Clicked, field: {}, action: {}",
                  getpid(),
                  static_cast<int>(_field),
                  static_cast<int>(_action));

    auto const cmd = _nowPlayingViewModel.resolveAction(_action, _field);

    using Type = uimodel::NowPlayingActionCommand::Type;

    switch (auto& commands = _runtime.playback().commands(); cmd.type)
    {
      case Type::Reveal: commands.revealPlayingTrack(); break;

      case Type::Pause: commands.pause(); break;

      case Type::Resume: commands.resume(); break;

      case Type::Navigate:
        APP_LOG_DEBUG("[PID {}] NowPlayingFieldLabel: Navigating to query: {}", getpid(), cmd.navigateQuery);
        std::ignore = _runtime.workspace().navigateTo(
          rt::FilteredListTarget{.listId = rt::kAllTracksListId, .filterExpression = cmd.navigateQuery});
        break;

      case Type::None:
      default: break;
    }
  }

  void NowPlayingFieldLabel::applyState(ao::uimodel::NowPlayingViewState const& view)
  {
    _label.set_text(uimodel::NowPlayingViewModel::fieldText(view, _field));
  }
} // namespace ao::gtk

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingStatusLabel.h"

#include "i18n/GtkTextCatalog.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <gdkmm/cursor.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>

#include <cstdint>
namespace ao::gtk
{
  NowPlayingStatusLabel::NowPlayingStatusLabel(rt::PlaybackService& playback, i18n::MessageCatalog const& textCatalog)
    : _commands{playback.commands()}
    , _nowPlayingViewModel{playback,
                           textCatalog,
                           [this](ao::uimodel::NowPlayingViewState const& view) { applyState(view); }}
  {
    _label.add_css_class("ao-nowplaying");
    _label.add_css_class("ao-clickable");
    _label.set_tooltip_text(gtkText(textCatalog, i18n::MessageId::GtkPlaybackShowPlayingList));

    auto const clickGesturePtr = Gtk::GestureClick::create();
    clickGesturePtr->signal_pressed().connect([this](std::int32_t, double, double) { _commands.revealPlayingTrack(); });

    _label.add_controller(clickGesturePtr);
    _label.set_cursor(Gdk::Cursor::create("pointer"));
  }

  NowPlayingStatusLabel::~NowPlayingStatusLabel() = default;

  void NowPlayingStatusLabel::applyState(ao::uimodel::NowPlayingViewState const& view)
  {
    _label.set_text(view.combinedStatus);
  }
} // namespace ao::gtk

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingStatusLabel.h"

#include "i18n/GtkTextCatalog.h"
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <gdkmm/cursor.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>

#include <cstdint>
#include <string>

namespace ao::gtk
{
  NowPlayingStatusLabel::NowPlayingStatusLabel(rt::PlaybackService& playback,
                                               uimodel::PresentationTextCatalog const& textCatalog,
                                               GtkTextCatalog const& gtkTextCatalog)
    : _commands{playback.commands()}
    , _nowPlayingViewModel{playback,
                           textCatalog,
                           [this](ao::uimodel::NowPlayingViewState const& view) { applyState(view); }}
  {
    _label.add_css_class("ao-nowplaying");
    _label.add_css_class("ao-clickable");
    _label.set_tooltip_text(std::string{gtkTextCatalog.text(GtkTextId::PlaybackShowPlayingList)});

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

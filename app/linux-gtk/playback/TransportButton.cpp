// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/TransportButton.h"

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/PlaybackQueueModel.h>
#include <ao/uimodel/playback/TransportViewModel.h>

#include <gtkmm/button.h>
#include <gtkmm/enums.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    char const* mapIconName(uimodel::playback::TransportIcon icon)
    {
      using Icon = uimodel::playback::TransportIcon;

      switch (icon)
      {
        case Icon::Play: return "media-playback-start-symbolic";
        case Icon::Pause: return "media-playback-pause-symbolic";
        case Icon::Stop: return "media-playback-stop-symbolic";
        case Icon::Next: return "media-skip-forward-symbolic";
        case Icon::Previous: return "media-skip-backward-symbolic";
        case Icon::Shuffle: return "media-playlist-shuffle-symbolic";
        case Icon::Repeat: return "media-playlist-repeat-symbolic";
        case Icon::RepeatOne: return "media-playlist-repeat-song-symbolic";
        default: return "";
      }
    }

    void applySizeClass(Gtk::Button& button, std::string const& size)
    {
      if (size == "small")
      {
        button.add_css_class("playback-button-small");
      }
      else if (size == "large")
      {
        button.add_css_class("playback-button-large");
      }
    }
  } // namespace

  TransportButton::TransportButton(rt::PlaybackService& playbackService,
                                   ao::uimodel::playback::PlaybackQueueModel* queueModel,
                                   Action action,
                                   std::function<void()> onPlaySelection,
                                   bool showLabel,
                                   std::string const& size)
  {
    _button.set_has_frame(false);
    _button.add_css_class("ao-playback-button");
    applySizeClass(_button, size);
    _button.set_valign(Gtk::Align::CENTER);

    _controller = std::make_unique<ao::uimodel::playback::TransportViewModel>(
      playbackService,
      queueModel,
      action,
      std::move(onPlaySelection),
      showLabel,
      [this](ao::uimodel::playback::TransportViewState const& state) { applyState(state); });

    _button.signal_clicked().connect([this] { _controller->handleClick(); });
  }

  TransportButton::~TransportButton() = default;

  void TransportButton::applyState(ao::uimodel::playback::TransportViewState const& view)
  {
    _button.set_icon_name(mapIconName(view.icon));
    _button.set_tooltip_text(view.tooltip);
    _button.set_sensitive(view.enabled);

    if (!view.label.empty())
    {
      _button.set_label(view.label);
    }

    if (view.playing)
    {
      _button.add_css_class("is-playing");
    }
    else
    {
      _button.remove_css_class("is-playing");
    }

    if (view.engaged)
    {
      _button.add_css_class("active");
    }
    else
    {
      _button.remove_css_class("active");
    }
  }
} // namespace ao::gtk

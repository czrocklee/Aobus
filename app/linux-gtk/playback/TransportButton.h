// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/TransportViewModel.h>

#include <gtkmm/button.h>
#include <gtkmm/widget.h>

#include <functional>
#include <string>

namespace ao::uimodel::playback
{
  class PlaybackQueueModel;
}

namespace ao::gtk
{
  class TransportButton final
  {
  public:
    using Action = uimodel::playback::TransportAction;

    TransportButton(rt::PlaybackService& playbackService,
                    uimodel::playback::PlaybackQueueModel* queueModel,
                    Action action,
                    std::function<void()> onPlaySelection = {},
                    bool showLabel = false,
                    std::string const& size = "normal");
    ~TransportButton();

    TransportButton(TransportButton const&) = delete;
    TransportButton& operator=(TransportButton const&) = delete;
    TransportButton(TransportButton&&) = delete;
    TransportButton& operator=(TransportButton&&) = delete;

    Gtk::Widget& widget() { return _button; }

  private:
    friend class TransportButtonTestPeer;

    void applyState(uimodel::playback::TransportViewState const& view);

    Gtk::Button _button;
    uimodel::playback::TransportViewModel _controller;
  };
} // namespace ao::gtk

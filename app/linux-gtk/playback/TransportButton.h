// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <gtkmm/button.h>
#include <gtkmm/widget.h>

#include <string>

namespace ao::uimodel
{
  class PlaybackCommandSurface;
}

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk
{
  class TransportButton final
  {
  public:
    using Action = uimodel::PlaybackCommand;

    TransportButton(rt::PlaybackService& playback,
                    uimodel::PlaybackCommandSurface& commands,
                    Action action,
                    bool showLabel = false,
                    std::string const& size = "normal");
    ~TransportButton();

    TransportButton(TransportButton const&) = delete;
    TransportButton& operator=(TransportButton const&) = delete;
    TransportButton(TransportButton&&) = delete;
    TransportButton& operator=(TransportButton&&) = delete;

    Gtk::Widget& widget() { return _button; }

  private:
    void applyState(uimodel::TransportViewState const& view);

    Gtk::Button _button;
    uimodel::TransportViewModel _transportViewModel;
  };
} // namespace ao::gtk

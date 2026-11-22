// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

namespace ao::rt
{
  class PlaybackService;
  class WorkspaceService;
}

namespace ao::uimodel
{
  class PlaybackActions;
}

namespace ao::gtk
{
  class NowPlayingFieldLabel final
  {
  public:
    using Action = uimodel::NowPlayingFieldAction;

    NowPlayingFieldLabel(rt::PlaybackService& playback,
                         rt::WorkspaceService& workspace,
                         uimodel::PlaybackActions* playbackActions,
                         i18n::MessageCatalog const& textCatalog,
                         rt::TrackField field,
                         Action action = Action::None);

    Gtk::Widget& widget() { return _label; }

  private:
    void applyState(uimodel::NowPlayingViewState const& view);
    void handleLabelClicked();

    rt::PlaybackService& _playback;
    rt::WorkspaceService& _workspace;
    uimodel::PlaybackActions* _playbackActions;
    rt::TrackField _field;
    Action _action;
    Gtk::Label _label;

    uimodel::NowPlayingViewModel _nowPlayingViewModel;
    sigc::scoped_connection _pressedConnection;
  };
} // namespace ao::gtk

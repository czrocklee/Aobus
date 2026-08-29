// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

namespace ao::rt
{
  class PlaybackService;
  class WorkspaceService;
}

namespace ao::gtk
{
  class NowPlayingFieldLabel final
  {
  public:
    using Action = uimodel::NowPlayingFieldAction;

    NowPlayingFieldLabel(rt::PlaybackService& playback,
                         rt::WorkspaceService& workspace,
                         i18n::MessageCatalog const& textCatalog,
                         rt::TrackField field,
                         Action action = Action::None);

    Gtk::Widget& widget() { return _label; }

  private:
    void applyState(uimodel::NowPlayingViewState const& view);
    void handleLabelClicked();

    rt::PlaybackService& _playback;
    rt::WorkspaceService& _workspace;
    rt::TrackField _field;
    Action _action;
    Gtk::Label _label;

    uimodel::NowPlayingViewModel _nowPlayingViewModel;
  };
} // namespace ao::gtk

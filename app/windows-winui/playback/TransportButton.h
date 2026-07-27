// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <memory>

namespace ao::winui
{
  struct WinUiDependencies;

  struct TransportButtonConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Button button{nullptr};
    uimodel::PlaybackCommand command = uimodel::PlaybackCommand::PlayPause;
    bool showLabel = false;
  };

  class TransportButton final
  {
  public:
    explicit TransportButton(TransportButtonConfig config);
    ~TransportButton();

    TransportButton(TransportButton const&) = delete;
    TransportButton& operator=(TransportButton const&) = delete;
    TransportButton(TransportButton&&) = delete;
    TransportButton& operator=(TransportButton&&) = delete;

    void bind(WinUiDependencies const& dependencies);
    void unbind();
    void activate();

  private:
    void applyState(uimodel::TransportViewState const& state);

    winrt::Microsoft::UI::Xaml::Controls::Button _button{nullptr};
    uimodel::PlaybackCommand _command = uimodel::PlaybackCommand::PlayPause;
    bool _showLabel = false;
    winrt::event_token _clickToken{};
    std::unique_ptr<uimodel::TransportViewModel> _viewModelPtr;
  };
} // namespace ao::winui

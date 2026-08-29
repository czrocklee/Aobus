// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <memory>

namespace ao::rt
{
  class PlaybackService;
} // namespace ao::rt

namespace ao::winui
{
  struct TransportButtonConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Button button{nullptr};
    i18n::MessageCatalog textCatalog;
    uimodel::PlaybackCommand command = uimodel::PlaybackCommand::PlayPause;
    bool showLabel = false;
  };

  class TransportButton final
  {
  public:
    TransportButton(TransportButtonConfig config, rt::PlaybackService& playback, uimodel::PlaybackActions& actions);
    ~TransportButton();

    TransportButton(TransportButton const&) = delete;
    TransportButton& operator=(TransportButton const&) = delete;
    TransportButton(TransportButton&&) = delete;
    TransportButton& operator=(TransportButton&&) = delete;

    void activate();

  private:
    /// Establish a blank state before the first model snapshot.
    void resetPresentation();

    void applyState(uimodel::TransportViewState const& state);

    winrt::Microsoft::UI::Xaml::Controls::Button _button{nullptr};
    i18n::MessageCatalog _textCatalog;
    uimodel::PlaybackCommand _command = uimodel::PlaybackCommand::PlayPause;
    bool _showLabel = false;
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _clickRevoker{};
    std::unique_ptr<uimodel::TransportViewModel> _viewModelPtr;
  };
} // namespace ao::winui

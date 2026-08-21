// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

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
    uimodel::PresentationTextCatalog textCatalog;
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

    void bind(rt::PlaybackService& playback, uimodel::PlaybackCommandSurface& commands);
    void unbind() noexcept;
    void activate();

  private:
    /// Blank the widget between bindings. Only a rebind has anything to show.
    void resetPresentation();

    void applyState(uimodel::TransportViewState const& state);

    winrt::Microsoft::UI::Xaml::Controls::Button _button{nullptr};
    uimodel::PresentationTextCatalog _textCatalog;
    uimodel::PlaybackCommand _command = uimodel::PlaybackCommand::PlayPause;
    bool _showLabel = false;
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _clickRevoker{};
    std::unique_ptr<uimodel::TransportViewModel> _viewModelPtr;
  };
} // namespace ao::winui

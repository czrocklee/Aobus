// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

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
  struct SoulTransportButtonConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Button button{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ContentControl soul{nullptr};
    uimodel::PresentationTextCatalog textCatalog;
    bool hasComplexTooltip = false;
    /// Whether the soul carries the play or pause glyph the transport state names.
    bool showGlyph = true;
    /// Whether a primary click runs play/pause, or the shell has claimed that gesture.
    bool activatesOnClick = true;
  };

  /**
   * @brief The soul, driven by the transport state and optionally driving it.
   *
   * The soul is the same visual in every shell, but only the shell that leaves
   * its primary click alone can use it as a transport control: a document that
   * binds an action to that slot owns the gesture, and the soul then merely
   * reports what playback is doing.
   */
  class SoulTransportButton final
  {
  public:
    explicit SoulTransportButton(SoulTransportButtonConfig config);
    ~SoulTransportButton();

    SoulTransportButton(SoulTransportButton const&) = delete;
    SoulTransportButton& operator=(SoulTransportButton const&) = delete;
    SoulTransportButton(SoulTransportButton&&) = delete;
    SoulTransportButton& operator=(SoulTransportButton&&) = delete;

    void bind(rt::PlaybackService& playback, uimodel::PlaybackCommandSurface& commands);
    void unbind() noexcept;
    void activate();

  private:
    /// Blank the widget between bindings. Only a rebind has anything to show.
    void resetPresentation();

    void applyState(uimodel::TransportViewState const& state);

    winrt::Microsoft::UI::Xaml::Controls::Button _button{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ContentControl _soul{nullptr};
    uimodel::PresentationTextCatalog _textCatalog;
    bool _hasComplexTooltip = false;
    bool _showGlyph = true;
    bool _activatesOnClick = true;
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _clickRevoker{};
    std::unique_ptr<uimodel::TransportViewModel> _viewModelPtr;
  };
} // namespace ao::winui

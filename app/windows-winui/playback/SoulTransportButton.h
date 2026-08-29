// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

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
    i18n::MessageCatalog textCatalog;
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
    SoulTransportButton(SoulTransportButtonConfig config,
                        rt::PlaybackService& playback,
                        uimodel::PlaybackActions& actions);
    ~SoulTransportButton();

    SoulTransportButton(SoulTransportButton const&) = delete;
    SoulTransportButton& operator=(SoulTransportButton const&) = delete;
    SoulTransportButton(SoulTransportButton&&) = delete;
    SoulTransportButton& operator=(SoulTransportButton&&) = delete;

    void activate();

  private:
    /// Establish a blank state before the first model snapshot.
    void resetPresentation();
    void stop() noexcept;

    void applyState(uimodel::TransportViewState const& state);

    winrt::Microsoft::UI::Xaml::Controls::Button _button{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ContentControl _soul{nullptr};
    i18n::MessageCatalog _textCatalog;
    bool _hasComplexTooltip = false;
    bool _showGlyph = true;
    bool _activatesOnClick = true;
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _clickRevoker{};
    std::unique_ptr<uimodel::TransportViewModel> _viewModelPtr;
  };
} // namespace ao::winui

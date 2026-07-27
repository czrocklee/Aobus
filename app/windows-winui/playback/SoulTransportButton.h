// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <memory>

namespace ao::winui
{
  struct WinUiDependencies;

  struct SoulTransportButtonConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Button button{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ContentControl soul{nullptr};
    bool hasComplexTooltip = false;
  };

  class SoulTransportButton final
  {
  public:
    explicit SoulTransportButton(SoulTransportButtonConfig config);
    ~SoulTransportButton();

    SoulTransportButton(SoulTransportButton const&) = delete;
    SoulTransportButton& operator=(SoulTransportButton const&) = delete;
    SoulTransportButton(SoulTransportButton&&) = delete;
    SoulTransportButton& operator=(SoulTransportButton&&) = delete;

    void bind(WinUiDependencies const& dependencies);
    void unbind();
    void activate();

  private:
    void applyState(uimodel::TransportViewState const& state);

    winrt::Microsoft::UI::Xaml::Controls::Button _button{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ContentControl _soul{nullptr};
    bool _hasComplexTooltip = false;
    winrt::event_token _clickToken{};
    std::unique_ptr<uimodel::TransportViewModel> _viewModelPtr;
  };
} // namespace ao::winui

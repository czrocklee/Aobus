// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

// Select the same classic COM ABI as the SMTC bridge.
#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#pragma pop_macro("GetCurrentTime")

#include <winrt/Windows.Media.h>

namespace ao::uimodel
{
  class PlaybackActions;
}

namespace ao::winui
{
  /// Handle a native SMTC button on the owner dispatcher. Unsupported buttons do nothing; there is no result reply.
  void dispatchSmtcButton(winrt::Windows::Media::SystemMediaTransportControlsButton button,
                          uimodel::PlaybackActions& actions);
} // namespace ao::winui

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <string_view>

namespace ao::winui
{
  void renderCoverArtPlaceholder(winrt::Microsoft::UI::Xaml::Controls::Grid const& root,
                                 uimodel::CoverArtPlaceholderPresentation const& presentation,
                                 std::string_view themeAccent);
} // namespace ao::winui

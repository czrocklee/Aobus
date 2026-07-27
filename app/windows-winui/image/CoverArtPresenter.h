// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "image/WindowsCoverArtLoader.h"
#include <ao/CoreIds.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ao::winui
{
  class WindowsThemeCoordinator;

  class CoverArtPresenter final
  {
  public:
    CoverArtPresenter(winrt::Microsoft::UI::Xaml::Controls::Image image,
                      winrt::Microsoft::UI::Xaml::Controls::Grid placeholder,
                      WindowsCoverArtLoader& loader,
                      WindowsThemeCoordinator& theme,
                      uimodel::CoverArtPlaceholderStyle style);
    ~CoverArtPresenter();

    CoverArtPresenter(CoverArtPresenter const&) = delete;
    CoverArtPresenter& operator=(CoverArtPresenter const&) = delete;
    CoverArtPresenter(CoverArtPresenter&&) = delete;
    CoverArtPresenter& operator=(CoverArtPresenter&&) = delete;

    void bind();
    void unbind();
    void select(ResourceId resourceId, uimodel::CoverArtPlaceholderIdentity identity, bool hasEntity);

  private:
    struct State;

    static void display(State& state, std::uint64_t generation, std::span<std::byte const> bytes);

    std::shared_ptr<State> _statePtr;
    WindowsCoverArtLoader& _loader;
    WindowsThemeCoordinator& _theme;
    WindowsCoverArtLoader::Request _request;
  };
} // namespace ao::winui

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <cstdint>
#include <memory>

namespace ao::winui
{
  class PreparedMemoryRandomAccessStream;
  class WindowsThemeCoordinator;

  class CoverArtPresenter final
  {
  public:
    CoverArtPresenter(winrt::Microsoft::UI::Xaml::Controls::Image image,
                      winrt::Microsoft::UI::Xaml::Controls::Grid placeholder,
                      rt::ResourceByteLoader& resources,
                      WindowsThemeCoordinator& theme,
                      uimodel::CoverArtPlaceholderStyle style);
    ~CoverArtPresenter();

    CoverArtPresenter(CoverArtPresenter const&) = delete;
    CoverArtPresenter& operator=(CoverArtPresenter const&) = delete;
    CoverArtPresenter(CoverArtPresenter&&) = delete;
    CoverArtPresenter& operator=(CoverArtPresenter&&) = delete;

    void bind(async::Runtime& runtime);
    void unbind();
    void select(ResourceId resourceId, uimodel::CoverArtPlaceholderIdentity identity, bool hasEntity);

  private:
    struct State;

    static async::Task<void> prepareAndDisplay(std::weak_ptr<State> statePtr,
                                               async::Runtime* runtime,
                                               std::uint64_t generation,
                                               rt::ResourceBytes bytes,
                                               std::stop_token stopToken);
    static void display(State& state, std::uint64_t generation, PreparedMemoryRandomAccessStream prepared);

    std::shared_ptr<State> _statePtr;
    rt::ResourceByteLoader& _resources;
    WindowsThemeCoordinator& _theme;
    rt::ResourceByteLoader::Request _request;
    async::Runtime* _runtime = nullptr;
    async::TaskHandle _streamTask;
  };
} // namespace ao::winui

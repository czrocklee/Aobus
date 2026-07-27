// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/uimodel/library/track/CoverArtRequestModel.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <memory>

namespace ao::rt
{
  class AppRuntime;
  class LibraryTaskService;
}

namespace ao::winui
{
  class CoverArtPresenter final
  {
  public:
    CoverArtPresenter(winrt::Microsoft::UI::Xaml::Controls::Image image,
                      winrt::Microsoft::UI::Xaml::UIElement placeholder);
    ~CoverArtPresenter();

    CoverArtPresenter(CoverArtPresenter const&) = delete;
    CoverArtPresenter& operator=(CoverArtPresenter const&) = delete;
    CoverArtPresenter(CoverArtPresenter&&) = delete;
    CoverArtPresenter& operator=(CoverArtPresenter&&) = delete;

    void bind(std::shared_ptr<rt::AppRuntime> runtimePtr);
    void unbind();
    void select(ResourceId resourceId);

  private:
    struct State;

    static async::Task<void> load(std::weak_ptr<State> state,
                                  rt::LibraryTaskService* tasks,
                                  async::Runtime* runtime,
                                  uimodel::CoverArtRequestToken token,
                                  std::stop_token stopToken);
    static void display(State& state, uimodel::CoverArtRequestToken token);

    std::shared_ptr<State> _statePtr;
    std::shared_ptr<rt::AppRuntime> _runtimePtr;
    async::TaskHandle _task;
  };
} // namespace ao::winui

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Media.h>

#include <memory>

struct HWND__;
using HWND = HWND__*;

namespace ao::rt
{
  class AppRuntime;
  class LibraryTaskService;
  struct PlaybackSnapshot;
}

namespace ao::uimodel
{
  struct CoverArtRequestToken;
  class PlaybackCommandSurface;
}

namespace ao::winui
{
  class SmtcBridge final
  {
  public:
    SmtcBridge(HWND window, winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher);
    ~SmtcBridge();

    SmtcBridge(SmtcBridge const&) = delete;
    SmtcBridge& operator=(SmtcBridge const&) = delete;
    SmtcBridge(SmtcBridge&&) = delete;
    SmtcBridge& operator=(SmtcBridge&&) = delete;

    void bind(std::shared_ptr<rt::AppRuntime> runtimePtr, uimodel::PlaybackCommandSurface& commands);
    void unbind();

  private:
    struct State;

    void handleSnapshot(rt::PlaybackSnapshot const& snapshot);
    void updateArtwork(ResourceId resourceId);
    static async::Task<void> updateArtworkWorkflow(std::weak_ptr<State> state,
                                                   rt::LibraryTaskService* tasks,
                                                   async::Runtime* runtime,
                                                   uimodel::CoverArtRequestToken token,
                                                   std::stop_token stopToken);
    static void writeArtworkStream(State& state, uimodel::CoverArtRequestToken token);

    std::shared_ptr<State> _statePtr;
    std::shared_ptr<rt::AppRuntime> _runtimePtr;
    async::Subscription _snapshotSub;
    async::TaskHandle _artworkTask;
  };
} // namespace ao::winui

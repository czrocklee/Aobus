// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/utility/ScopedRegistration.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Media.h>

#include <memory>
#include <stop_token>

struct HWND__;
using HWND = HWND__*;

namespace ao::rt
{
  class AppRuntime;
  class ResourceByteLoader;
  struct PlaybackSnapshot;
}

namespace ao::uimodel
{
  class PlaybackCommandSurface;
}

namespace ao::winui
{
  class PreparedMemoryRandomAccessStream;
  class SmtcBridge final
  {
  public:
    SmtcBridge(HWND window, winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher);
    ~SmtcBridge();

    SmtcBridge(SmtcBridge const&) = delete;
    SmtcBridge& operator=(SmtcBridge const&) = delete;
    SmtcBridge(SmtcBridge&&) = delete;
    SmtcBridge& operator=(SmtcBridge&&) = delete;

    void bind(std::shared_ptr<rt::AppRuntime> runtimePtr,
              uimodel::PlaybackCommandSurface& commands,
              rt::ResourceByteLoader& resourceBytes);
    void unbind();

  private:
    struct State;

    void handleSnapshot(rt::PlaybackSnapshot const& snapshot);
    void updateArtwork(ResourceId resourceId);
    static async::Task<void> prepareAndWriteArtwork(std::weak_ptr<State> statePtr,
                                                    std::shared_ptr<rt::AppRuntime> runtimePtr,
                                                    ResourceId resourceId,
                                                    rt::ResourceBytes bytes,
                                                    std::stop_token stopToken);
    static void writeArtworkStream(State& state, ResourceId resourceId, PreparedMemoryRandomAccessStream prepared);

    std::shared_ptr<State> _statePtr;
    std::shared_ptr<rt::AppRuntime> _runtimePtr;
    rt::ResourceByteLoader* _resourceBytes = nullptr;
    async::Subscription _snapshotSub;
    utility::ScopedRegistration _artworkRequest;
    async::TaskHandle _artworkTask;
  };
} // namespace ao::winui

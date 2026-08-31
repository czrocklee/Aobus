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
  class ResourceByteMemoryCache;
  struct PlaybackSnapshot;
}

namespace ao::uimodel
{
  class PlaybackActions;
}

namespace ao::winui
{
  class PreparedMemoryRandomAccessStream;
  class SmtcBridge final
  {
  public:
    SmtcBridge(HWND window,
               winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
               rt::AppRuntime& runtime,
               uimodel::PlaybackActions& actions,
               rt::ResourceByteMemoryCache& resourceBytes);
    ~SmtcBridge();

    SmtcBridge(SmtcBridge const&) = delete;
    SmtcBridge& operator=(SmtcBridge const&) = delete;
    SmtcBridge(SmtcBridge&&) = delete;
    SmtcBridge& operator=(SmtcBridge&&) = delete;

  private:
    struct State;

    static void retireNativeSession(State& state) noexcept;
    void handleSnapshot(rt::PlaybackSnapshot const& snapshot);
    void updateArtwork(ResourceId resourceId);
    static async::Task<void> prepareAndWriteArtwork(std::weak_ptr<State> statePtr,
                                                    async::Runtime* runtime,
                                                    ResourceId resourceId,
                                                    rt::ResourceBytes bytes,
                                                    std::stop_token stopToken);
    static void writeArtworkStream(State& state, ResourceId resourceId, PreparedMemoryRandomAccessStream prepared);

    std::shared_ptr<State> _statePtr;
    rt::AppRuntime& _runtime;
    rt::ResourceByteMemoryCache& _resourceBytes;
    async::Subscription _snapshotSub;
    utility::ScopedRegistration _artworkRequest;
    async::TaskHandle _artworkTask;
    // Declared last so failed-constructor unwinding closes native admission
    // before cancelling any partially established callback work.
    utility::ScopedRegistration _nativeSessionRetirement;
  };
} // namespace ao::winui

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/runtime/AsyncTestSupport.h"
#include <ao/async/Task.h>
#include <ao/audio/BackendProvider.h>
#include <ao/rt/AppRuntime.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace ao::async
{
  class Executor;
  class Sleeper;
}

namespace ao::test
{
  class TempDir;
}

namespace ao::rt
{
  class ConfigStore;
  class PlaybackTransport;
  class TextOrderingPolicy;
}

namespace ao::rt::test
{
  audio::BackendProvider::Status makeReadyAudioStatus();
  audio::BackendProvider::Status makePipeWireOutputStatus();
  std::unique_ptr<audio::BackendProvider> makeReadyAudioProvider();
  std::unique_ptr<audio::BackendProvider> makeReadyAudioProvider(audio::BackendProvider::Status status);

  void addReadyAudioProvider(PlaybackTransport& transport);
  void addReadyAudioProvider(PlaybackTransport& transport, audio::BackendProvider::Status status);
  void addReadyAudioProvider(AppRuntime& runtime);
  void addReadyAudioProvider(AppRuntime& runtime, audio::BackendProvider::Status status);

  std::unique_ptr<AppRuntime> makeRuntime(ao::test::TempDir const& tempDir,
                                          std::unique_ptr<async::Executor> executorPtr,
                                          ConfigStore* playbackSessionConfigStore = nullptr,
                                          async::Sleeper* sleeper = nullptr,
                                          TextOrderingPolicy const* textOrderingPolicy = nullptr);

  // State-only helper. Tests that can produce asynchronous callbacks must use
  // makeRuntime and provide an executor with the required scheduling model.
  std::unique_ptr<AppRuntime> makeStateOnlyRuntime(ao::test::TempDir const& tempDir,
                                                   ConfigStore* playbackSessionConfigStore = nullptr);

  void settleRuntimeCallbacks(AppRuntime& runtime);

  template<typename T>
  T runRuntimeTask(AppRuntime& runtime, async::Task<T> task)
  {
    return runTestTask(runtime.async(), runtime.async().callbackExecutor(), std::move(task));
  }

  template<typename T, typename Pump>
  T runRuntimeTask(AppRuntime& runtime, async::Task<T> task, Pump& pump)
  {
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = runtime.async().spawn(flagCompletion(completedPtr, std::move(task)));
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};

    while (!completedPtr->load() && std::chrono::steady_clock::now() < deadline)
    {
      pump();

      if (!completedPtr->load())
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
    }

    REQUIRE(completedPtr->load());
    return detail::finishDrivenTask(future, [&pump] { pump(); });
  }
} // namespace ao::rt::test

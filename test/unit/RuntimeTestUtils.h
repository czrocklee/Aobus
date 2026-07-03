// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Executor.h>
#include <ao/audio/Backend.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Subscription.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/PlaybackService.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ao::rt::test
{
  inline audio::IBackendProvider::Status makeReadyAudioStatus()
  {
    return {.metadata =
              {
                .id = audio::BackendId{"test_backend"},
                .name = "Test Backend",
                .supportedProfiles =
                  {
                    {.id = audio::kProfileShared, .name = "Shared", .description = "Shared profile"},
                  },
              },
            .devices = {
              audio::Device{.id = audio::DeviceId{"test_device"},
                            .displayName = "Test Device",
                            .description = "Ready test output",
                            .isDefault = true,
                            .backendId = audio::BackendId{"test_backend"}},
            }};
  }

  inline audio::IBackendProvider::Status makePipeWireOutputStatus()
  {
    return {
      .metadata =
        {
          .id = audio::BackendId{"pipewire"},
          .name = "PipeWire",
          .description = "PipeWire Sound Server",
          .iconName = "pipewire",
          .supportedProfiles =
            {
              {.id = audio::kProfileShared, .name = "Shared", .description = "Shared mode"},
              {.id = audio::kProfileExclusive, .name = "Exclusive", .description = "Exclusive mode"},
            },
        },
      .devices =
        {
          {
            .id = audio::DeviceId{"device1"},
            .displayName = "Built-in Audio",
            .description = "Built-in analog stereo",
            .isDefault = true,
            .backendId = audio::BackendId{"pipewire"},
          },
        },
    };
  }

  namespace detail
  {
    struct ReadyAudioBackend final : audio::NullBackend
    {
      audio::BackendId backendIdValue;
      audio::ProfileId profileIdValue;

      ReadyAudioBackend(audio::BackendId backendId, audio::ProfileId profileId)
        : backendIdValue{std::move(backendId)}, profileIdValue{std::move(profileId)}
      {
      }

      audio::BackendId backendId() const noexcept override { return backendIdValue; }
      audio::ProfileId profileId() const noexcept override { return profileIdValue; }
    };

    struct ReadyAudioProvider final : audio::IBackendProvider
    {
      Status provStatus;

      explicit ReadyAudioProvider(Status status)
        : provStatus{std::move(status)}
      {
      }

      ReadyAudioProvider()
        : provStatus{makeReadyAudioStatus()}
      {
      }

      void shutdown() noexcept override {}

      audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        if (callback)
        {
          callback(provStatus.devices);
        }

        return audio::Subscription{};
      }

      Status status() const override { return provStatus; }

      std::unique_ptr<audio::IBackend> createBackend(audio::Device const& device,
                                                     audio::ProfileId const& profile) override
      {
        return std::make_unique<ReadyAudioBackend>(device.backendId, profile);
      }

      audio::Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override
      {
        return audio::Subscription{};
      }
    };
  } // namespace detail

  inline std::unique_ptr<audio::IBackendProvider> makeReadyAudioProvider()
  {
    return std::make_unique<detail::ReadyAudioProvider>();
  }

  inline std::unique_ptr<audio::IBackendProvider> makeReadyAudioProvider(audio::IBackendProvider::Status status)
  {
    return std::make_unique<detail::ReadyAudioProvider>(std::move(status));
  }

  inline void addReadyAudioProvider(PlaybackService& playback)
  {
    playback.addProvider(makeReadyAudioProvider());
  }

  inline void addReadyAudioProvider(PlaybackService& playback, audio::IBackendProvider::Status status)
  {
    playback.addProvider(makeReadyAudioProvider(std::move(status)));
  }

  class TestMusicLibrary final
  {
  public:
    TestMusicLibrary()
      : _tempDir{}, _library{_tempDir.path(), _tempDir.path()}
    {
    }

    library::MusicLibrary& library() { return _library; }
    library::MusicLibrary const& library() const { return _library; }
    std::filesystem::path const& root() const { return _tempDir.path(); }

    TrackId addTrack(library::test::TrackSpec const& spec) { return library::test::addTrack(_library, spec); }

    void updateTrack(TrackId id, std::move_only_function<void(library::test::TrackSpec&)> updater)
    {
      library::test::updateTrackSpec(_library, id, std::move(updater));
    }

    TrackId addTrack(std::string_view title) { return addTrack(library::test::TrackSpec{.title = std::string{title}}); }

  private:
    ao::test::TempDir _tempDir;
    library::MusicLibrary _library;
  };

  /**
   * @brief Lifecycle-safe test state tracker.
   */
  template<typename T>
  class AsyncTestState final
  {
  public:
    static auto create(T initial) { return AsyncTestState{std::make_shared<std::atomic<T>>(initial)}; }

    void set(T value) { *_dataPtr = value; }
    T get() const { return _dataPtr->load(); }

    bool waitUntil(T expected, std::chrono::milliseconds timeout = std::chrono::milliseconds{500})
    {
      auto start = std::chrono::steady_clock::now();

      while (std::chrono::steady_clock::now() - start < timeout)
      {
        if (get() == expected)
        {
          return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }

      return get() == expected;
    }

    std::atomic<T>* operator->() { return _dataPtr.get(); }
    std::atomic<T>& operator*() { return *_dataPtr; }

  private:
    explicit AsyncTestState(std::shared_ptr<std::atomic<T>> dataPtr)
      : _dataPtr{std::move(dataPtr)}
    {
    }

    std::shared_ptr<std::atomic<T>> _dataPtr;
  };

  /**
   * @brief Deterministic barrier for testing.
   * Blocks the current thread (intended for use in worker threads).
   */
  class AsyncBarrier final
  {
  public:
    void wait()
    {
      auto lock = std::unique_lock{_mutex};

      _cv.wait(lock, [this] { return _released; });
    }

    void release()
    {
      {
        auto const lock = std::scoped_lock{_mutex};
        _released = true;
      }

      _cv.notify_all();
    }

  private:
    bool _released{false};
    std::mutex _mutex;
    std::condition_variable _cv;
  };

  class ManualExecutor : public async::IExecutor
  {
  public:
    bool isCurrent() const noexcept override { return true; }

    void dispatch(std::move_only_function<void()> task) override
    {
      auto const lock = std::scoped_lock{_mutex};
      _tasks.push_back(std::move(task));
    }

    void defer(std::move_only_function<void()> task) override { dispatch(std::move(task)); }

    bool runOne()
    {
      auto task = std::move_only_function<void()>{};

      {
        auto const lock = std::scoped_lock{_mutex};

        if (_tasks.empty())
        {
          return false;
        }

        task = std::move(_tasks.front());
        _tasks.pop_front();
      }

      task();
      return true;
    }

    void runUntilIdle()
    {
      while (runOne())
      {
      }
    }

    std::size_t queuedCount() const
    {
      auto const lock = std::scoped_lock{_mutex};
      return _tasks.size();
    }

    bool waitUntilQueued(std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) const
    {
      auto const deadline = std::chrono::steady_clock::now() + timeout;

      while (std::chrono::steady_clock::now() < deadline)
      {
        if (queuedCount() != 0)
        {
          return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }

      return queuedCount() != 0;
    }

    void expectQueued(std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) const
    {
      INFO("Timed out waiting for queued executor task");
      REQUIRE(waitUntilQueued(timeout));
    }

  protected:
    std::mutex& taskMutex() noexcept { return _mutex; }
    std::deque<std::move_only_function<void()>>& queuedTasks() noexcept { return _tasks; }

  private:
    mutable std::mutex _mutex;
    std::deque<std::move_only_function<void()>> _tasks;
  };

  /**
   * @brief Immediate executor for tests — runs tasks synchronously on the calling thread.
   */
  class MockExecutor final : public async::IExecutor
  {
  public:
    bool isCurrent() const noexcept override { return true; }
    void dispatch(std::move_only_function<void()> task) override { task(); }
    void defer(std::move_only_function<void()> task) override { task(); }
  };

  class QueuedExecutor final : public ManualExecutor
  {
  public:
    bool isCurrent() const noexcept override { return std::this_thread::get_id() == _ownerThreadId; }

    void dispatch(std::move_only_function<void()> task) override
    {
      {
        auto const lock = std::scoped_lock{taskMutex()};
        queuedTasks().push_back(std::move(task));
      }

      _cv.notify_all();
    }

    void drain() { runUntilIdle(); }

    bool waitUntilQueued(std::chrono::milliseconds timeout = std::chrono::seconds{2})
    {
      auto lock = std::unique_lock{taskMutex()};
      return _cv.wait_for(lock, timeout, [this] { return !queuedTasks().empty(); });
    }

    template<typename Predicate>
    bool drainUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds{2})
    {
      auto const deadline = std::chrono::steady_clock::now() + timeout;

      while (!predicate())
      {
        auto const now = std::chrono::steady_clock::now();

        if (now >= deadline)
        {
          return predicate();
        }

        auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

        if (!waitUntilQueued(remaining))
        {
          return predicate();
        }

        drain();
      }

      return true;
    }

  private:
    std::thread::id _ownerThreadId = std::this_thread::get_id();
    std::condition_variable _cv;
  };

  /**
   * @brief Creates an AppRuntime backed by a temporary directory with a MockExecutor.
   */
  inline auto makeRuntime(ao::test::TempDir const& tempDir)
  {
    return AppRuntime{AppRuntimeDependencies{
      .executorPtr = std::make_unique<MockExecutor>(),
      .musicRoot = tempDir.path(),
      .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
      .workspaceConfigStorePtr =
        std::make_unique<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml"),
    }};
  }
} // namespace ao::rt::test

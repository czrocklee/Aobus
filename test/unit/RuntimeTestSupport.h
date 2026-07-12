// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Executor.h>
#include <ao/async/Runtime.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Subscription.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/PlaybackService.h>

#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/use_awaitable.hpp>
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

namespace ao::async
{
  class RuntimeTestAccess final
  {
  public:
    template<typename SleepFor>
    static void setSleepFor(Runtime& runtime, SleepFor&& sleepFor)
    {
      runtime._sleepForOverride = std::forward<SleepFor>(sleepFor);
    }
  };
} // namespace ao::async

namespace ao::rt::test
{
  class ControlledSleeper final
  {
  public:
    using Delay = std::chrono::milliseconds;

    struct Call final
    {
      std::uint64_t id = 0;
      Delay delay{};
      bool cancelled = false;
      std::thread::id startedOn;
      std::thread::id cancelledOn;
    };

    static ControlledSleeper& install(async::Runtime& runtime)
    {
      auto sleeperPtr = std::shared_ptr<ControlledSleeper>{new ControlledSleeper{}};
      async::RuntimeTestAccess::setSleepFor(
        runtime, [sleeperPtr](Delay const delay) { return sleeperPtr->sleepFor(delay); });
      return *sleeperPtr;
    }

    ~ControlledSleeper() = default;

    ControlledSleeper(ControlledSleeper const&) = delete;
    ControlledSleeper& operator=(ControlledSleeper const&) = delete;
    ControlledSleeper(ControlledSleeper&&) = delete;
    ControlledSleeper& operator=(ControlledSleeper&&) = delete;

    async::Task<void> sleepFor(Delay const delay)
    {
      co_await boost::asio::async_initiate<decltype(boost::asio::use_awaitable), void()>(
        [this, delay](auto handler)
        {
          auto const id = _nextId.fetch_add(1);
          auto cancellationSlot = boost::asio::get_associated_cancellation_slot(handler);

          if (cancellationSlot.is_connected())
          {
            cancellationSlot.assign(
              [this, id](async::CancellationType)
              {
                auto const lock = std::scoped_lock{_mutex};

                if (auto const it = entry(id); it != _entries.end())
                {
                  it->cancelled = true;
                  it->cancelledOn = std::this_thread::get_id();
                  it->active = false;
                }

                _cv.notify_all();
              });
          }

          {
            auto const lock = std::scoped_lock{_mutex};
            _entries.push_back(Entry{.id = id,
                                     .delay = delay,
                                     .resume = [handler = std::move(handler)] mutable { handler(); },
                                     .active = true,
                                     .startedOn = std::this_thread::get_id(),
                                     .cancelledOn = {}});
          }

          _cv.notify_all();
        },
        boost::asio::use_awaitable);
    }

    bool waitForCallCount(std::size_t const count,
                          std::chrono::milliseconds const timeout = std::chrono::seconds{2}) const
    {
      auto lock = std::unique_lock{_mutex};
      return _cv.wait_for(lock, timeout, [this, count] { return _entries.size() >= count; });
    }

    std::size_t callCount() const
    {
      auto const lock = std::scoped_lock{_mutex};
      return _entries.size();
    }

    Call call(std::size_t const index) const
    {
      auto lock = std::unique_lock{_mutex};
      _cv.wait(lock, [this, index] { return _entries.size() > index; });
      auto const& entryValue = _entries[index];
      return {.id = entryValue.id,
              .delay = entryValue.delay,
              .cancelled = entryValue.cancelled,
              .startedOn = entryValue.startedOn,
              .cancelledOn = entryValue.cancelledOn};
    }

    bool waitForCancellation(std::size_t const index,
                             std::chrono::milliseconds const timeout = std::chrono::seconds{2}) const
    {
      auto lock = std::unique_lock{_mutex};
      return _cv.wait_for(
        lock, timeout, [this, index] { return _entries.size() > index && _entries[index].cancelled; });
    }

    bool fire(std::size_t const index, bool const ignoreCancellation = false)
    {
      auto resume = std::move_only_function<void()>{};

      {
        auto const lock = std::scoped_lock{_mutex};

        if (index >= _entries.size() || (!_entries[index].active && !ignoreCancellation) || !_entries[index].resume)
        {
          return false;
        }

        _entries[index].active = false;
        resume = std::move(_entries[index].resume);
      }

      resume();
      return true;
    }

    bool fireNext()
    {
      std::size_t index = 0;

      {
        auto lock = std::unique_lock{_mutex};

        if (!_cv.wait_for(
              lock, std::chrono::seconds{2}, [this] { return std::ranges::any_of(_entries, &Entry::active); }))
        {
          return false;
        }

        index = static_cast<std::size_t>(std::ranges::find(_entries, true, &Entry::active) - _entries.begin());
      }

      return fire(index);
    }

    bool fireNext(Delay const delay)
    {
      std::size_t index = 0;

      {
        auto lock = std::unique_lock{_mutex};

        if (!_cv.wait_for(lock,
                          std::chrono::seconds{2},
                          [this, delay]
                          {
                            return std::ranges::any_of(_entries,
                                                       [delay](Entry const& candidate)
                                                       { return candidate.active && candidate.delay == delay; });
                          }))
        {
          return false;
        }

        auto const it = std::ranges::find_if(
          _entries, [delay](Entry const& candidate) { return candidate.active && candidate.delay == delay; });
        index = static_cast<std::size_t>(it - _entries.begin());
      }

      return fire(index);
    }

    bool forceFire(std::uint64_t const id)
    {
      std::size_t index = 0;

      {
        auto const lock = std::scoped_lock{_mutex};
        auto const it = entry(id);

        if (it == _entries.end())
        {
          return false;
        }

        index = static_cast<std::size_t>(it - _entries.begin());
      }

      return fire(index, true);
    }

    std::uint64_t lastScheduledId() const
    {
      auto lock = std::unique_lock{_mutex};
      _cv.wait(lock, [this] { return !_entries.empty(); });
      return _entries.back().id;
    }

    std::vector<Delay> pendingDelays() const
    {
      auto const lock = std::scoped_lock{_mutex};
      auto delays = std::vector<Delay>{};

      for (auto const& candidate : _entries)
      {
        if (candidate.active)
        {
          delays.push_back(candidate.delay);
        }
      }

      return delays;
    }

    bool waitForPendingDelays(std::vector<Delay> const& expected,
                              std::chrono::milliseconds const timeout = std::chrono::seconds{2}) const
    {
      auto lock = std::unique_lock{_mutex};
      return _cv.wait_for(lock, timeout, [this, &expected] { return pendingDelaysLocked() == expected; });
    }

    bool waitForPendingDelay(Delay const delay, std::chrono::milliseconds const timeout = std::chrono::seconds{2}) const
    {
      auto lock = std::unique_lock{_mutex};
      return _cv.wait_for(lock,
                          timeout,
                          [this, delay]
                          {
                            return std::ranges::any_of(_entries,
                                                       [delay](Entry const& candidate)
                                                       { return candidate.active && candidate.delay == delay; });
                          });
    }

  private:
    ControlledSleeper() = default;

    struct Entry final
    {
      std::uint64_t id = 0;
      Delay delay{};
      std::move_only_function<void()> resume;
      bool active = false;
      bool cancelled = false;
      std::thread::id startedOn;
      std::thread::id cancelledOn;
    };

    std::vector<Entry>::iterator entry(std::uint64_t const id) { return std::ranges::find(_entries, id, &Entry::id); }

    std::vector<Delay> pendingDelaysLocked() const
    {
      auto delays = std::vector<Delay>{};

      for (auto const& candidate : _entries)
      {
        if (candidate.active)
        {
          delays.push_back(candidate.delay);
        }
      }

      return delays;
    }

    mutable std::mutex _mutex;
    mutable std::condition_variable _cv;
    std::vector<Entry> _entries;
    std::atomic_uint64_t _nextId{1};
  };

  inline audio::BackendProvider::Status makeReadyAudioStatus()
  {
    return {.descriptor =
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

  inline audio::BackendProvider::Status makePipeWireOutputStatus()
  {
    return {
      .descriptor =
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

      audio::BackendId backendId() const override { return backendIdValue; }
      audio::ProfileId profileId() const override { return profileIdValue; }
    };

    struct ReadyAudioProvider final : audio::BackendProvider
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

      std::unique_ptr<audio::Backend> createBackend(audio::Device const& device,
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

  inline std::unique_ptr<audio::BackendProvider> makeReadyAudioProvider()
  {
    return std::make_unique<detail::ReadyAudioProvider>();
  }

  inline std::unique_ptr<audio::BackendProvider> makeReadyAudioProvider(audio::BackendProvider::Status status)
  {
    return std::make_unique<detail::ReadyAudioProvider>(std::move(status));
  }

  inline void addReadyAudioProvider(PlaybackService& playback)
  {
    playback.addProvider(makeReadyAudioProvider());
  }

  inline void addReadyAudioProvider(PlaybackService& playback, audio::BackendProvider::Status status)
  {
    playback.addProvider(makeReadyAudioProvider(std::move(status)));
  }

  class MusicLibraryFixture final
  {
  public:
    MusicLibraryFixture()
      : _tempDir{}, _library{library::test::makeTestMusicLibrary(_tempDir.path(), _tempDir.path())}
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
    static auto create(T initial) { return AsyncTestState{std::make_shared<Data>(initial)}; }

    void set(T value) const
    {
      {
        auto const lock = std::scoped_lock{_dataPtr->mutex};
        _dataPtr->value.store(value);
      }

      _dataPtr->cv.notify_all();
    }

    T increment() const
    {
      T result = {};

      {
        auto const lock = std::scoped_lock{_dataPtr->mutex};
        result = _dataPtr->value.fetch_add(1) + 1;
      }

      _dataPtr->cv.notify_all();
      return result;
    }

    T load() const { return _dataPtr->value.load(); }

    bool waitUntil(T expected, std::chrono::milliseconds timeout = std::chrono::seconds{2}) const
    {
      auto lock = std::unique_lock{_dataPtr->mutex};
      return _dataPtr->cv.wait_for(lock, timeout, [this, expected] { return load() == expected; });
    }

  private:
    struct Data final
    {
      explicit Data(T initial)
        : value{initial}
      {
      }

      std::atomic<T> value;
      std::mutex mutex;
      std::condition_variable cv;
    };

    explicit AsyncTestState(std::shared_ptr<Data> dataPtr)
      : _dataPtr{std::move(dataPtr)}
    {
    }

    std::shared_ptr<Data> _dataPtr;
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

  class ManualExecutor : public async::Executor
  {
  public:
    bool isCurrent() const noexcept override { return true; }

    void dispatch(std::move_only_function<void()> task) override
    {
      {
        auto const lock = std::scoped_lock{_mutex};
        _tasks.push_back(std::move(task));
      }

      _cv.notify_all();
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

    bool waitUntilQueued(std::chrono::milliseconds timeout = std::chrono::seconds{2}) const
    {
      auto lock = std::unique_lock{_mutex};
      return _cv.wait_for(lock, timeout, [this] { return !_tasks.empty(); });
    }

    void checkQueued(std::chrono::milliseconds timeout = std::chrono::seconds{2}) const
    {
      INFO("Timed out waiting for queued executor task");
      REQUIRE(waitUntilQueued(timeout));
    }

  private:
    mutable std::mutex _mutex;
    std::deque<std::move_only_function<void()>> _tasks;
    mutable std::condition_variable _cv;
  };

  /**
   * @brief Immediate executor for tests — runs tasks synchronously on the calling thread.
   */
  class MockExecutor final : public async::Executor
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

    void dispatch(std::move_only_function<void()> task) override { ManualExecutor::dispatch(std::move(task)); }

    void drain() { runUntilIdle(); }

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
  };

  inline auto makeRuntime(ao::test::TempDir const& tempDir,
                          std::unique_ptr<async::Executor> executorPtr,
                          ConfigStore* playbackSessionConfigStore = nullptr)
  {
    return AppRuntime{AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = tempDir.path(),
      .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
      .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
      .workspaceConfigStorePtr =
        std::make_unique<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml"),
      .playbackSessionConfigStore = playbackSessionConfigStore,
    }};
  }

  /**
   * @brief Creates an AppRuntime backed by a temporary directory with a MockExecutor.
   */
  inline auto makeRuntime(ao::test::TempDir const& tempDir, ConfigStore* playbackSessionConfigStore = nullptr)
  {
    return makeRuntime(tempDir, std::make_unique<MockExecutor>(), playbackSessionConfigStore);
  }
} // namespace ao::rt::test

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Executor.h>
#include <ao/async/LoopExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Sleeper.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Player.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Subscription.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibraryWriter.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  struct RecordedAsyncException final
  {
    std::exception_ptr exceptionPtr;
    std::string context;
  };

  class AsyncExceptionRecorder final
  {
  public:
    async::AsyncExceptionHandler handler()
    {
      return [this](std::exception_ptr exceptionPtr, std::string_view const context)
      {
        auto const lock = std::scoped_lock{_mutex};
        _exceptions.push_back({.exceptionPtr = std::move(exceptionPtr), .context = std::string{context}});
        _cv.notify_all();
      };
    }

    bool waitForCount(std::size_t const count, std::chrono::milliseconds const timeout = std::chrono::seconds{2}) const
    {
      auto lock = std::unique_lock{_mutex};
      return _cv.wait_for(lock, timeout, [this, count] { return _exceptions.size() >= count; });
    }

    std::vector<RecordedAsyncException> snapshot() const
    {
      auto const lock = std::scoped_lock{_mutex};
      return _exceptions;
    }

  private:
    mutable std::mutex _mutex;
    mutable std::condition_variable _cv;
    std::vector<RecordedAsyncException> _exceptions;
  };

  template<typename ExceptionType>
  bool isExceptionType(std::exception_ptr const& exceptionPtr)
  {
    if (!exceptionPtr)
    {
      return false;
    }

    try
    {
      std::rethrow_exception(exceptionPtr);
    }
    catch (ExceptionType const&)
    {
      return true;
    }
    catch (...)
    {
      return false;
    }
  }

  template<typename ExceptionType>
  void checkRecordedException(RecordedAsyncException const& exception, std::string_view const expectedContext)
  {
    CHECK(exception.context == expectedContext);
    CHECK(isExceptionType<ExceptionType>(exception.exceptionPtr));
  }

  template<typename ExceptionType>
  void requireSingleRecordedException(AsyncExceptionRecorder const& recorder, std::string_view const expectedContext)
  {
    auto const exceptions = recorder.snapshot();
    REQUIRE(exceptions.size() == 1);
    checkRecordedException<ExceptionType>(exceptions.front(), expectedContext);
  }

  template<typename T>
  std::exception_ptr captureTaskFutureException(async::TaskFuture<T>& future)
  {
    try
    {
      if constexpr (std::is_void_v<T>)
      {
        future.get();
      }
      else
      {
        std::ignore = future.get();
      }
    }
    catch (...)
    {
      // Keep ownership explicit while tests inspect a cross-thread exception;
      // GCC ThreadSanitizer cannot model ownership held by an active catch.
      return std::current_exception();
    }

    return {};
  }

  inline PlaybackService makePlaybackService(async::Executor& executor,
                                             library::MusicLibrary& library,
                                             NotificationService& notifications)
  {
    return PlaybackService{executor, library, notifications, std::make_unique<audio::Player>(executor)};
  }

  // Injectable delay strategy for tests: pass a pointer to one into the Runtime
  // (directly, or via makeRuntime/AppRuntimeDependencies) at construction, then
  // drive its pending sleeps deterministically. The Sleeper must outlive the
  // Runtime, so declare it before the Runtime it feeds.
  class ControlledSleeper final : public async::Sleeper
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

    ControlledSleeper() = default;
    ~ControlledSleeper() override = default;

    ControlledSleeper(ControlledSleeper const&) = delete;
    ControlledSleeper& operator=(ControlledSleeper const&) = delete;
    ControlledSleeper(ControlledSleeper&&) = delete;
    ControlledSleeper& operator=(ControlledSleeper&&) = delete;

    async::Task<void> sleepFor(Delay const delay, std::stop_token const stopToken) override
    {
      auto executor = co_await boost::asio::this_coro::executor;
      auto timerExecutor = boost::asio::make_strand(executor);
      co_await boost::asio::co_spawn(timerExecutor, waitForSignal(delay, stopToken), boost::asio::use_awaitable);
    }

    bool waitForCallCount(std::size_t const count,
                          std::chrono::milliseconds const timeout = std::chrono::seconds{2}) const
    {
      auto lock = std::unique_lock{_mutex};
      return _cv.wait_for(lock,
                          timeout,
                          [this, count]
                          {
                            if (_entries.size() < count)
                            {
                              return false;
                            }

                            for (std::size_t index = 0; index < count; ++index)
                            {
                              if (!_entries[index].published)
                              {
                                return false;
                              }
                            }

                            return true;
                          });
    }

    std::size_t callCount() const
    {
      auto const lock = std::scoped_lock{_mutex};
      return static_cast<std::size_t>(std::ranges::count(_entries, true, &Entry::published));
    }

    Call call(std::size_t const index) const
    {
      auto lock = std::unique_lock{_mutex};
      _cv.wait(lock, [this, index] { return _entries.size() > index && _entries[index].published; });
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
        lock,
        timeout,
        [this, index] { return _entries.size() > index && _entries[index].published && _entries[index].cancelled; });
    }

    bool fire(std::size_t const index)
    {
      auto timerPtr = std::shared_ptr<boost::asio::steady_timer>{};

      {
        auto const lock = std::scoped_lock{_mutex};

        if (index >= _entries.size() || !_entries[index].published || !_entries[index].active)
        {
          return false;
        }

        timerPtr = _entries[index].timerPtr.lock();

        if (timerPtr == nullptr)
        {
          _entries[index].active = false;
          return false;
        }

        _entries[index].active = false;
      }

      boost::asio::dispatch(timerPtr->get_executor(), [timerPtr] { std::ignore = timerPtr->cancel(); });
      return true;
    }

    bool fireNext()
    {
      std::size_t index = 0;

      {
        auto lock = std::unique_lock{_mutex};

        if (!_cv.wait_for(lock,
                          std::chrono::seconds{2},
                          [this]
                          {
                            return std::ranges::any_of(
                              _entries, [](Entry const& candidate) { return candidate.published && candidate.active; });
                          }))
        {
          return false;
        }

        auto const it = std::ranges::find_if(
          _entries, [](Entry const& candidate) { return candidate.published && candidate.active; });
        index = static_cast<std::size_t>(it - _entries.begin());
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
                            return std::ranges::any_of(
                              _entries,
                              [delay](Entry const& candidate)
                              { return candidate.published && candidate.active && candidate.delay == delay; });
                          }))
        {
          return false;
        }

        auto const it =
          std::ranges::find_if(_entries,
                               [delay](Entry const& candidate)
                               { return candidate.published && candidate.active && candidate.delay == delay; });
        index = static_cast<std::size_t>(it - _entries.begin());
      }

      return fire(index);
    }

    bool fireById(std::uint64_t const id)
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

      return fire(index);
    }

    std::uint64_t lastScheduledId() const
    {
      auto lock = std::unique_lock{_mutex};
      _cv.wait(lock, [this] { return std::ranges::any_of(_entries, &Entry::published); });
      auto const it = std::ranges::find_if(_entries | std::views::reverse, &Entry::published);
      return it->id;
    }

    std::vector<Delay> pendingDelays() const
    {
      auto const lock = std::scoped_lock{_mutex};
      auto delays = std::vector<Delay>{};

      for (auto const& candidate : _entries)
      {
        if (candidate.published && candidate.active)
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
                            return std::ranges::any_of(
                              _entries,
                              [delay](Entry const& candidate)
                              { return candidate.published && candidate.active && candidate.delay == delay; });
                          });
    }

  private:
    using StopCallback = std::stop_callback<std::function<void()>>;

    async::Task<void> waitForSignal(Delay const delay, std::stop_token const stopToken)
    {
      auto executor = co_await boost::asio::this_coro::executor;
      auto timerPtr = std::make_shared<boost::asio::steady_timer>(executor);
      timerPtr->expires_at(std::chrono::steady_clock::time_point::max());
      auto const id = _nextId.fetch_add(1);

      {
        auto const lock = std::scoped_lock{_mutex};
        _entries.push_back(Entry{
          .id = id, .delay = delay, .timerPtr = timerPtr, .active = true, .startedOn = std::this_thread::get_id()});
      }

      auto stopCallback = StopCallback{
        stopToken,
        [this, id, timerPtr]
        {
          bool wonCancellation = false;

          {
            auto const lock = std::scoped_lock{_mutex};

            if (auto const it = entry(id); it != _entries.end() && it->active)
            {
              it->cancelled = true;
              it->cancelledOn = std::this_thread::get_id();
              it->active = false;
              wonCancellation = true;
            }
          }

          _cv.notify_all();

          if (wonCancellation)
          {
            boost::asio::dispatch(timerPtr->get_executor(), [timerPtr] { std::ignore = timerPtr->cancel(); });
          }
        }};

      {
        auto const lock = std::scoped_lock{_mutex};
        entry(id)->published = true;
      }

      _cv.notify_all();

      if (stopToken.stop_requested())
      {
        co_return;
      }

      try
      {
        co_await timerPtr->async_wait(boost::asio::use_awaitable);
      }
      catch (boost::system::system_error const& error)
      {
        if (error.code() != boost::asio::error::operation_aborted)
        {
          throw;
        }
      }
    }

    struct Entry final
    {
      std::uint64_t id = 0;
      Delay delay{};
      std::weak_ptr<boost::asio::steady_timer> timerPtr;
      bool active = false;
      // Public test-driver methods observe an entry only after its stop callback
      // is registered, so cancellation cannot race the helper's publication.
      bool published = false;
      bool cancelled = false;
      std::thread::id startedOn = {};
      std::thread::id cancelledOn = {};
    };

    std::vector<Entry>::iterator entry(std::uint64_t const id) { return std::ranges::find(_entries, id, &Entry::id); }

    std::vector<Delay> pendingDelaysLocked() const
    {
      auto delays = std::vector<Delay>{};

      for (auto const& candidate : _entries)
      {
        if (candidate.published && candidate.active)
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
                .supportedProfiles =
                  {
                    {.id = audio::kProfileShared},
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
          .supportedProfiles =
            {
              {.id = audio::kProfileShared},
              {.id = audio::kProfileExclusive},
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

  inline MetadataPatch metadataPatch(library::test::TrackSpec const& spec)
  {
    auto patch = MetadataPatch{
      .optTitle = spec.title,
      .optArtist = spec.artist,
      .optAlbum = spec.album,
      .optAlbumArtist = spec.albumArtist,
      .optGenre = spec.genre,
      .optComposer = spec.composer,
      .optConductor = spec.conductor,
      .optEnsemble = spec.ensemble,
      .optWork = spec.work,
      .optMovement = spec.movement,
      .optSoloist = spec.soloist,
      .optYear = spec.year,
      .optTrackNumber = spec.trackNumber,
      .optTrackTotal = spec.trackTotal,
      .optDiscNumber = spec.discNumber,
      .optDiscTotal = spec.discTotal,
      .optMovementNumber = spec.movementNumber,
      .optMovementTotal = spec.movementTotal,
    };

    for (auto const& [key, value] : spec.customMetadata)
    {
      patch.customUpdates.emplace(key, value);
    }

    return patch;
  }

  inline TrackId addRuntimeTrack(AppRuntime& runtime,
                                 library::test::TrackSpec const& spec,
                                 std::move_only_function<void()> settlePublication = {})
  {
    static auto nextFixtureTrack = std::atomic<std::uint64_t>{0};
    auto sourcePath = std::filesystem::path{spec.uri};

    if (sourcePath.is_relative())
    {
      sourcePath = runtime.musicRoot() / sourcePath;
    }

    if (!std::filesystem::is_regular_file(sourcePath))
    {
      sourcePath = audio::test::requireAudioFixture("basic_metadata.flac");
    }

    auto const sequence = nextFixtureTrack.fetch_add(1, std::memory_order_relaxed);
    auto const relativePath =
      std::filesystem::path{".aobus-test"} / std::format("track-{}{}", sequence, sourcePath.extension().string());
    auto const destinationPath = runtime.musicRoot() / relativePath;
    std::filesystem::create_directories(destinationPath.parent_path());
    std::filesystem::copy_file(sourcePath, destinationPath, std::filesystem::copy_options::overwrite_existing);

    auto& writer = runtime.library().writer();
    auto createResult = writer.createTrackFromFile(destinationPath);
    REQUIRE(createResult);
    auto const trackId = createResult->trackId;

    if (settlePublication)
    {
      settlePublication();
    }

    auto bindingResult = runtime.library().bindTrackTargets(std::span{&trackId, std::size_t{1}});
    REQUIRE(bindingResult);
    auto targets = std::move(*bindingResult);
    auto patchResult = writer.updateMetadata(targets, metadataPatch(spec));
    REQUIRE(patchResult);
    REQUIRE(
      (patchResult->status == TrackAuthoringStatus::Applied || patchResult->status == TrackAuthoringStatus::NoOp));

    if (settlePublication)
    {
      settlePublication();
    }

    if (!spec.tags.empty())
    {
      if (patchResult->optNextTargets)
      {
        targets = *patchResult->optNextTargets;
      }

      auto tagResult = writer.editTags(targets, spec.tags, std::span<std::string const>{});
      REQUIRE(tagResult);
      REQUIRE((tagResult->status == TrackAuthoringStatus::Applied || tagResult->status == TrackAuthoringStatus::NoOp));

      if (settlePublication)
      {
        settlePublication();
      }
    }

    REQUIRE(spec.coverArtId == kInvalidResourceId);
    return trackId;
  }

  inline void updateRuntimeTrack(AppRuntime& runtime,
                                 TrackId const trackId,
                                 std::move_only_function<void(library::test::TrackSpec&)> updater)
  {
    auto spec = library::test::TrackSpec{};
    {
      auto transaction = runtime.musicLibrary().readTransaction();
      auto optView =
        runtime.musicLibrary().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      spec = library::test::trackSpecFromView(runtime.musicLibrary(), *optView);
    }

    updater(spec);
    REQUIRE(spec.coverArtId == kInvalidResourceId);
    auto bindingResult = runtime.library().bindTrackTargets(std::span{&trackId, std::size_t{1}});
    REQUIRE(bindingResult);
    auto result = runtime.library().writer().updateMetadata(*bindingResult, metadataPatch(spec));
    REQUIRE(result);
    REQUIRE((result->status == TrackAuthoringStatus::Applied || result->status == TrackAuthoringStatus::NoOp));
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
      // A waiter may destroy the object containing this AsyncTestState as soon
      // as it observes the value. Keep notify under the lock so wait_for cannot
      // return until this method has made its final access through `this`.
      auto const lock = std::scoped_lock{_dataPtr->mutex};
      _dataPtr->value.store(value);
      _dataPtr->cv.notify_all();
    }

    T increment() const
    {
      T result = {};

      // See set() for the lifetime synchronization provided by the lock.
      auto const lock = std::scoped_lock{_dataPtr->mutex};
      result = _dataPtr->value.fetch_add(1) + 1;
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

  class ManualExecutor final : public async::Executor
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

  // Test-only executor that collapses dispatch and defer onto the calling
  // stack. Use it only when turn and cross-thread behavior are out of scope.
  class InlineExecutor final : public async::Executor
  {
  public:
    bool isCurrent() const noexcept override { return true; }
    void dispatch(std::move_only_function<void()> task) override { task(); }
    void defer(std::move_only_function<void()> task) override { task(); }
  };

  class LibraryWriterFixture final
  {
  public:
    LibraryWriterFixture(library::MusicLibrary& storage, LibraryChanges& changes)
      : _asyncRuntime{_executor}, _storage{storage}, _changes{changes}
    {
    }

    ~LibraryWriterFixture()
    {
      _libraryPtr.reset();
      _asyncRuntime.requestStop();
      _asyncRuntime.join();
    }

    LibraryWriterFixture(LibraryWriterFixture const&) = delete;
    LibraryWriterFixture& operator=(LibraryWriterFixture const&) = delete;
    LibraryWriterFixture(LibraryWriterFixture&&) = delete;
    LibraryWriterFixture& operator=(LibraryWriterFixture&&) = delete;

    Library& library() { return ensureLibrary(); }
    auto& writer() { return ensureLibrary().writer(); }
    BoundTrackTargets bind(std::span<TrackId const> trackIds)
    {
      return ao::test::requireValue(ensureLibrary().bindTrackTargets(trackIds));
    }

    Result<UpdateTrackMetadataReply> updateMetadata(std::span<TrackId const> trackIds, MetadataPatch const& patch)
    {
      auto bindingResult = ensureLibrary().bindTrackTargets(trackIds);

      if (!bindingResult)
      {
        return std::unexpected{bindingResult.error()};
      }

      auto outcomeResult = ensureLibrary().writer().updateMetadata(*bindingResult, patch);

      if (!outcomeResult)
      {
        return std::unexpected{outcomeResult.error()};
      }

      switch (outcomeResult->status)
      {
        case TrackAuthoringStatus::Applied:
        case TrackAuthoringStatus::NoOp: return std::move(outcomeResult->reply);
        case TrackAuthoringStatus::Missing:
          return makeError(Error::Code::NotFound, "Track authoring target is missing");
        case TrackAuthoringStatus::Stale: return makeError(Error::Code::Conflict, "Track authoring binding is stale");
        case TrackAuthoringStatus::Unavailable:
          return makeError(Error::Code::InvalidState, "Track authoring is unavailable");
      }

      return makeError(Error::Code::InvalidState, "Unknown track authoring status");
    }

    Result<EditTrackTagsReply> editTags(std::span<TrackId const> trackIds,
                                        std::span<std::string const> tagsToAdd,
                                        std::span<std::string const> tagsToRemove)
    {
      auto bindingResult = ensureLibrary().bindTrackTargets(trackIds);

      if (!bindingResult)
      {
        return std::unexpected{bindingResult.error()};
      }

      auto outcomeResult = ensureLibrary().writer().editTags(*bindingResult, tagsToAdd, tagsToRemove);

      if (!outcomeResult)
      {
        return std::unexpected{outcomeResult.error()};
      }

      switch (outcomeResult->status)
      {
        case TrackAuthoringStatus::Applied:
        case TrackAuthoringStatus::NoOp: return std::move(outcomeResult->reply);
        case TrackAuthoringStatus::Missing:
          return makeError(Error::Code::NotFound, "Track authoring target is missing");
        case TrackAuthoringStatus::Stale: return makeError(Error::Code::Conflict, "Track authoring binding is stale");
        case TrackAuthoringStatus::Unavailable:
          return makeError(Error::Code::InvalidState, "Track authoring is unavailable");
      }

      return makeError(Error::Code::InvalidState, "Unknown track authoring status");
    }

  private:
    Library& ensureLibrary()
    {
      if (!_libraryPtr)
      {
        _libraryPtr = std::make_unique<Library>(_asyncRuntime, _storage, _changes);
      }

      return *_libraryPtr;
    }

    InlineExecutor _executor;
    async::Runtime _asyncRuntime;
    library::MusicLibrary& _storage;
    LibraryChanges& _changes;
    std::unique_ptr<Library> _libraryPtr;
  };

  // Deterministic test adapter over the production loop executor. Dispatch is
  // deliberately queued so tests can inspect state before delivering a turn.
  class QueuedExecutor final : public async::Executor
  {
  public:
    bool isCurrent() const noexcept override { return _loopExecutor.isCurrent(); }

    void dispatch(std::move_only_function<void()> task) override { enqueue(std::move(task)); }
    void defer(std::move_only_function<void()> task) override { enqueue(std::move(task)); }

    void drain()
    {
      while (_loopExecutor.runReadyTurn())
      {
      }
    }

    template<typename Predicate>
    bool drainUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds{2})
    {
      auto const deadline = std::chrono::steady_clock::now() + timeout;

      while (!predicate())
      {
        if (_loopExecutor.runReadyTurn())
        {
          continue;
        }

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
      }

      return true;
    }

    std::size_t queuedCount() const
    {
      auto const lock = std::scoped_lock{_mutex};
      return _queuedCount;
    }

    bool waitUntilQueued(std::chrono::milliseconds timeout = std::chrono::seconds{2}) const
    {
      auto lock = std::unique_lock{_mutex};
      return _cv.wait_for(lock, timeout, [this] { return _queuedCount != 0; });
    }

    void checkQueued(std::chrono::milliseconds timeout = std::chrono::seconds{2}) const
    {
      INFO("Timed out waiting for queued executor task");
      REQUIRE(waitUntilQueued(timeout));
    }

  private:
    void enqueue(std::move_only_function<void()> task)
    {
      if (!task)
      {
        return;
      }

      {
        auto const lock = std::scoped_lock{_mutex};
        ++_queuedCount;

        try
        {
          _loopExecutor.defer(
            [this, task = std::move(task)] mutable
            {
              {
                auto const taskLock = std::scoped_lock{_mutex};
                --_queuedCount;
              }

              task();
            });
        }
        catch (...)
        {
          --_queuedCount;
          throw;
        }
      }

      _cv.notify_all();
    }

    mutable std::mutex _mutex;
    mutable std::condition_variable _cv;
    std::size_t _queuedCount = 0;
    async::LoopExecutor _loopExecutor;
  };

  template<typename Predicate>
  bool runLoopUntil(async::LoopExecutor& executor,
                    Predicate predicate,
                    std::chrono::milliseconds timeout = std::chrono::seconds{5})
  {
    auto const deadline = std::chrono::steady_clock::now() + timeout;

    while (!predicate() && std::chrono::steady_clock::now() < deadline)
    {
      if (!executor.runReadyTurn())
      {
        std::this_thread::yield();
      }
    }

    return predicate();
  }

  inline bool driveRenderUntilTaskQueued(audio::RenderTarget& renderTarget,
                                         QueuedExecutor& executor,
                                         std::span<std::byte> output,
                                         std::chrono::milliseconds timeout = std::chrono::seconds{5})
  {
    auto const deadline = std::chrono::steady_clock::now() + timeout;

    while (executor.queuedCount() == 0)
    {
      renderTarget.renderPcm(output);
      auto const now = std::chrono::steady_clock::now();

      if (now >= deadline)
      {
        return false;
      }

      auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      auto const pollInterval = std::min(remaining, std::chrono::milliseconds{1});

      if (executor.waitUntilQueued(pollInterval))
      {
        return true;
      }
    }

    return true;
  }

  inline auto makeRuntime(ao::test::TempDir const& tempDir,
                          std::unique_ptr<async::Executor> executorPtr,
                          ConfigStore* playbackSessionConfigStore = nullptr,
                          async::Sleeper* sleeper = nullptr)
  {
    return AppRuntime{AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = tempDir.path(),
      .databasePath = LibraryPaths{tempDir.path()}.databasePath(),
      .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
      .workspaceConfigStorePtr =
        std::make_unique<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml"),
      .playbackSessionConfigStore = playbackSessionConfigStore,
      .sleeper = sleeper,
    }};
  }

  /**
   * @brief Creates an AppRuntime backed by a temporary directory with an InlineExecutor.
   */
  inline auto makeRuntime(ao::test::TempDir const& tempDir,
                          ConfigStore* playbackSessionConfigStore = nullptr,
                          async::Sleeper* sleeper = nullptr)
  {
    return makeRuntime(tempDir, std::make_unique<InlineExecutor>(), playbackSessionConfigStore, sleeper);
  }
} // namespace ao::rt::test

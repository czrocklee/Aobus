// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include <ao/Type.h>
#include <ao/async/Executor.h>
#include <ao/audio/Backend.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Subscription.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
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
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::rt::test
{
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

      ReadyAudioProvider()
      {
        provStatus.metadata.id = audio::BackendId{"test_backend"};
        provStatus.metadata.name = "Test Backend";
        provStatus.metadata.supportedProfiles.push_back(
          {.id = audio::kProfileShared, .name = "Shared", .description = "Shared profile"});
        provStatus.devices.push_back(audio::Device{.id = audio::DeviceId{"test_device"},
                                                   .displayName = "Test Device",
                                                   .description = "Ready test output",
                                                   .isDefault = true,
                                                   .backendId = audio::BackendId{"test_backend"}});
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

  inline void addReadyAudioProvider(PlaybackService& playback)
  {
    playback.addProvider(std::make_unique<detail::ReadyAudioProvider>());
  }

  struct TrackSpec final
  {
    std::string title = "Track";
    std::string artist = "Artist";
    std::string album = "Album";
    std::string albumArtist{};
    std::string genre{};
    std::string composer{};
    std::string work{};
    std::string movement{};
    std::uint16_t year = 2020;
    std::uint16_t discNumber = 1;
    std::uint16_t trackNumber = 1;
    std::uint16_t movementNumber = 0;
    std::uint16_t movementTotal = 0;
    std::chrono::milliseconds duration = std::chrono::seconds{200};
  };

  inline TrackSpec makeSpec(std::string_view title, std::uint16_t year)
  {
    return TrackSpec{.title = std::string{title}, .year = year};
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

    TrackId addTrack(TrackSpec const& spec)
    {
      auto txn = _library.writeTransaction();
      auto writer = _library.tracks().writer(txn);
      auto builder = library::TrackBuilder::createNew();
      builder.metadata()
        .title(spec.title)
        .artist(spec.artist)
        .album(spec.album)
        .albumArtist(spec.albumArtist)
        .genre(spec.genre)
        .composer(spec.composer)
        .work(spec.work)
        .movement(spec.movement)
        .year(spec.year)
        .discNumber(spec.discNumber)
        .trackNumber(spec.trackNumber)
        .movementNumber(spec.movementNumber)
        .movementTotal(spec.movementTotal);
      builder.property()
        .uri("/tmp/test.flac")
        .duration(spec.duration)
        .bitrate(Bitrate{320000})
        .sampleRate(SampleRate{44100})
        .channels(Channels{2})
        .bitDepth(BitDepth{16});
      auto hotData = builder.serializeHot(txn, _library.dictionary());
      REQUIRE(hotData);
      auto coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
      REQUIRE(coldData);
      auto createResult = writer.createHotCold(*hotData, *coldData);
      REQUIRE(createResult);
      auto const [id, _] = *createResult;
      REQUIRE(txn.commit());
      return id;
    }

    void updateTrack(TrackId id, std::move_only_function<void(TrackSpec&)> updater)
    {
      auto txn = _library.writeTransaction();
      auto reader = _library.tracks().reader(txn);
      auto writer = _library.tracks().writer(txn);

      auto optViewResult = reader.get(id, library::TrackStore::Reader::LoadMode::Both);

      if (!optViewResult)
      {
        return;
      }

      auto spec =
        TrackSpec{.title = std::string{optViewResult->metadata().title()},
                  .artist = std::string{_library.dictionary().getOrDefault(optViewResult->metadata().artistId())},
                  .album = std::string{_library.dictionary().getOrDefault(optViewResult->metadata().albumId())},
                  .work = std::string{_library.dictionary().getOrDefault(optViewResult->metadata().workId())},
                  .year = optViewResult->metadata().year()};

      updater(spec);

      auto builder = library::TrackBuilder::createNew();
      builder.metadata().title(spec.title).artist(spec.artist).album(spec.album).work(spec.work).year(spec.year);

      auto hotData = builder.serializeHot(txn, _library.dictionary());
      REQUIRE(hotData);
      auto coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
      REQUIRE(coldData);

      REQUIRE(writer.updateHot(id, *hotData));
      REQUIRE(writer.updateCold(
        id, coldData->size(), [&](std::span<std::byte> buf) { std::ranges::copy(*coldData, buf.begin()); }));
      REQUIRE(txn.commit());
    }

    TrackId addTrack(std::string_view title) { return addTrack(TrackSpec{.title = std::string{title}}); }

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

  class ManualExecutor final : public async::IExecutor
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

  /**
   * @brief Lightweight test utility that records rendered states.
   */
  template<typename TState>
  struct RenderLog final
  {
    std::vector<TState> states;

    void render(TState const& state) { states.push_back(state); }

    TState const& last() const { return states.back(); }

    bool empty() const { return states.empty(); }

    void clear() { states.clear(); }
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

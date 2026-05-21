// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackBuilder.h"
#include "ao/library/TrackStore.h"
#include "test/unit/lmdb/TestUtils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace ao::rt::test
{
  struct TrackSpec final
  {
    std::string title = "Track";
    std::string artist = "Artist";
    std::string album = "Album";
    std::string albumArtist{};
    std::string genre{};
    std::string composer{};
    std::string work{};
    std::uint16_t year = 2020;
    std::uint16_t discNumber = 1;
    std::uint16_t trackNumber = 1;
    std::uint32_t durationMs = 200000;
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
        .year(spec.year)
        .discNumber(spec.discNumber)
        .trackNumber(spec.trackNumber);
      builder.property()
        .uri("/tmp/test.flac")
        .durationMs(spec.durationMs)
        .bitrate(320000)
        .sampleRate(44100)
        .channels(2)
        .bitDepth(16);
      auto hotData = builder.serializeHot(txn, _library.dictionary());
      auto coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
      auto [id, _] = writer.createHotCold(hotData, coldData);
      txn.commit();
      return id;
    }

    TrackId addTrack(std::string_view title) { return addTrack(TrackSpec{.title = std::string{title}}); }

  private:
    lmdb::test::TempDir _tempDir;
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

    void set(T value) { *_data = value; }
    T get() const { return _data->load(); }

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

    std::atomic<T>* operator->() { return _data.get(); }
    std::atomic<T>& operator*() { return *_data; }

  private:
    explicit AsyncTestState(std::shared_ptr<std::atomic<T>> data)
      : _data{std::move(data)}
    {
    }

    std::shared_ptr<std::atomic<T>> _data;
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
} // namespace ao::rt::test

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "ScriptedDecoderSession.h"
#include <ao/AudioCodec.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Format.h>
#include <ao/audio/PlaybackInput.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  inline Device makeEngineTestDevice(std::string_view id = "test-device")
  {
    return {.id = DeviceId{std::string{id}},
            .displayName = "Test",
            .description = "Test",
            .isDefault = false,
            .backendId = kBackendNone};
  }

  inline Format makeEngineTestFormat()
  {
    return {.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
  }

  inline Engine::PlaybackItem makePlaybackItem(std::filesystem::path path)
  {
    static auto nextId = std::atomic<std::uint64_t>{1};
    return Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = nextId.fetch_add(1, std::memory_order_relaxed)},
      .input = PlaybackInput{.filePath = std::move(path)},
    };
  }

  inline Engine::PlaybackItem makePlaybackItem(PlaybackInput input)
  {
    static auto nextId = std::atomic<std::uint64_t>{100000};
    return Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = nextId.fetch_add(1, std::memory_order_relaxed)},
      .input = std::move(input),
    };
  }

  inline auto makeScriptedEngineDecoderFactory(Format fmt = makeEngineTestFormat())
  {
    return [fmt](auto const&, auto const&)
    {
      auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::milliseconds{0}, .isLossy = false});
      auto data = std::vector(100, std::byte{0});

      decPtr->setReadScript({{.data = data, .endOfStream = false}, {.data = {}, .endOfStream = true}});
      return decPtr;
    };
  }

  struct ScriptedTrack final
  {
    std::filesystem::path path;
    DecodedStreamInfo info;
    std::vector<std::byte> data;
  };

  inline DecodedStreamInfo makeScriptedStreamInfo(Format format,
                                                  AudioCodec codec = AudioCodec::Flac,
                                                  bool isLossy = false)
  {
    return {.sourceFormat = format,
            .outputFormat = format,
            .duration = std::chrono::milliseconds{10},
            .isLossy = isLossy,
            .codec = codec};
  }

  inline auto makePathScriptedDecoderFactory(std::vector<ScriptedTrack> tracks)
  {
    return [tracks = std::move(tracks)](std::filesystem::path const& path, Format const&)
    {
      for (auto const& track : tracks)
      {
        if (track.path == path)
        {
          auto decPtr = std::make_unique<ScriptedDecoderSession>(track.info);
          decPtr->setReadScript({{.data = track.data, .endOfStream = false}, {.data = {}, .endOfStream = true}});
          return decPtr;
        }
      }

      return std::unique_ptr<ScriptedDecoderSession>{};
    };
  }

  // Tracks how many decoder sessions are live. `TrackSession::create` opens a
  // short-lived probe decoder and then the streaming decoder per track, so
  // only `live()` measured at a settled point (never mid-open) equals the
  // number of streaming sources currently alive.
  struct DecoderLifeCounters final
  {
    std::atomic<std::size_t> created{0};
    std::atomic<std::size_t> destroyed{0};

    std::size_t live() const
    {
      return created.load(std::memory_order_relaxed) - destroyed.load(std::memory_order_relaxed);
    }
  };

  // Like makePathScriptedDecoderFactory, but every decoder it creates bumps the
  // shared life counters, so a test can observe that retired gapless sources
  // are reclaimed rather than accumulated across a continuous splice run.
  inline auto makeCountingDecoderFactory(std::vector<ScriptedTrack> tracks,
                                         std::shared_ptr<DecoderLifeCounters> countersPtr)
  {
    return [tracks = std::move(tracks), countersPtr = std::move(countersPtr)](
             std::filesystem::path const& path, Format const&)
    {
      for (auto const& track : tracks)
      {
        if (track.path == path)
        {
          countersPtr->created.fetch_add(1, std::memory_order_relaxed);
          auto destroyCounterPtr = std::shared_ptr<std::atomic<std::size_t>>{countersPtr, &countersPtr->destroyed};
          auto decPtr = std::make_unique<ScriptedDecoderSession>(track.info);
          decPtr->setReadScript({{.data = track.data, .endOfStream = false}, {.data = {}, .endOfStream = true}});
          decPtr->setDestroyCounter(std::move(destroyCounterPtr));
          return decPtr;
        }
      }

      return std::unique_ptr<ScriptedDecoderSession>{};
    };
  }

  // A ScriptedTrack plus an optional post-seek read script (see
  // ScriptedDecoderSession::setSeekReadScript).
  struct RegisteredTrack final
  {
    ScriptedTrack track;
    std::optional<std::vector<ScriptedDecoderSession::ReadScriptEntry>> optSeekScript;
  };

  // Like makePathScriptedDecoderFactory, but records the most recently created
  // decoder per path. TrackSession opens a short-lived probe decoder first and
  // then the streaming decoder, so the last entry per path is the decoder
  // playback actually reads. Observation only: the engine owns the decoders,
  // so a recorded pointer may only be dereferenced while its track's source is
  // provably still alive.
  inline auto makeRegisteringDecoderFactory(
    std::vector<RegisteredTrack> tracks,
    std::shared_ptr<std::map<std::filesystem::path, ScriptedDecoderSession*>> registryPtr)
  {
    return [tracks = std::move(tracks), registryPtr = std::move(registryPtr)](
             std::filesystem::path const& path, Format const&)
    {
      for (auto const& entry : tracks)
      {
        if (entry.track.path == path)
        {
          auto decPtr = std::make_unique<ScriptedDecoderSession>(entry.track.info);
          decPtr->setReadScript({{.data = entry.track.data, .endOfStream = false}, {.data = {}, .endOfStream = true}});

          if (entry.optSeekScript)
          {
            decPtr->setSeekReadScript(*entry.optSeekScript);
          }

          (*registryPtr)[path] = decPtr.get();
          return decPtr;
        }
      }

      return std::unique_ptr<ScriptedDecoderSession>{};
    };
  }

  class CallbackLatch final
  {
  public:
    void notify()
    {
      auto const lock = std::scoped_lock{_mutex};
      ++_count;
      _cv.notify_all();
    }

    bool waitForCount(std::size_t expected, std::chrono::milliseconds timeout = std::chrono::seconds{1})
    {
      auto lock = std::unique_lock{_mutex};
      return _cv.wait_for(lock, timeout, [this, expected] { return _count >= expected; });
    }

    std::size_t count() const
    {
      auto const lock = std::scoped_lock{_mutex};
      return _count;
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _cv;
    std::size_t _count = 0;
  };
} // namespace ao::audio::test

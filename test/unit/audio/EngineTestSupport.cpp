// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"

#include "ScriptedDecoderSession.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PlaybackInput.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  Device makeEngineTestDevice(std::string_view id)
  {
    return {.id = DeviceId{std::string{id}},
            .displayName = "Test",
            .description = "Test",
            .isDefault = false,
            .backendId = kBackendNone};
  }

  Format makeEngineTestFormat()
  {
    return {.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
  }

  Engine::PlaybackItem makePlaybackItem(std::filesystem::path path)
  {
    static auto nextId = std::atomic<std::uint64_t>{1};
    return Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = nextId.fetch_add(1, std::memory_order_relaxed)},
      .input = PlaybackInput{.filePath = std::move(path)},
    };
  }

  Engine::PlaybackItem makePlaybackItem(PlaybackInput input)
  {
    static auto nextId = std::atomic<std::uint64_t>{100000};
    return Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = nextId.fetch_add(1, std::memory_order_relaxed)},
      .input = std::move(input),
    };
  }

  DecoderFactoryFn makeScriptedEngineDecoderFactory(Format fmt)
  {
    return [fmt](std::filesystem::path const&, Format)
    {
      auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::milliseconds{0}, .isLossy = false});
      auto data = std::vector(100, std::byte{0});

      decPtr->setReadScript({{.data = data, .endOfStream = false}, {.endOfStream = true}});
      return decPtr;
    };
  }

  DecodedStreamInfo makeScriptedStreamInfo(Format format, AudioCodec codec, bool isLossy)
  {
    return {.sourceFormat = format,
            .outputFormat = format,
            .duration = std::chrono::milliseconds{10},
            .isLossy = isLossy,
            .codec = codec};
  }

  DecoderFactoryFn makePathScriptedDecoderFactory(std::vector<ScriptedTrack> tracks)
  {
    return [tracks = std::move(tracks)](std::filesystem::path const& path, Format)
    {
      for (auto const& track : tracks)
      {
        if (track.path == path)
        {
          auto decPtr = std::make_unique<ScriptedDecoderSession>(track.info);
          decPtr->setReadScript({{.data = track.data, .endOfStream = false}, {.endOfStream = true}});
          return decPtr;
        }
      }

      return std::unique_ptr<ScriptedDecoderSession>{};
    };
  }

  std::size_t DecoderLifeCounters::live() const
  {
    return created.load(std::memory_order_relaxed) - destroyed.load(std::memory_order_relaxed);
  }

  void BlockingPreparationGate::enterAndWait()
  {
    if (blocked.exchange(true, std::memory_order_relaxed))
    {
      return;
    }

    entered.release();
    release.acquire();
  }

  bool BlockingPreparationGate::waitForEntry(std::chrono::milliseconds timeout)
  {
    return entered.try_acquire_for(timeout);
  }

  DecoderFactoryFn makeBlockingPreparationDecoderFactory(std::shared_ptr<BlockingPreparationGate> gatePtr,
                                                         std::filesystem::path blockedPath)
  {
    return
      [gatePtr = std::move(gatePtr), blockedPath = std::move(blockedPath)](std::filesystem::path const& path, Format)
    {
      auto const isBlockedPath =
        path == blockedPath || (!blockedPath.has_parent_path() && path.filename() == blockedPath);

      if (isBlockedPath)
      {
        gatePtr->enterAndWait();
      }

      auto const format = makeEngineTestFormat();
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = format,
        .outputFormat = format,
        .duration = std::chrono::seconds{2},
        .isLossy = false,
        .codec = AudioCodec::Flac,
      });
      decoderPtr->setReadScript(
        {{.data = std::vector<std::byte>(100000, std::byte{0}), .endOfStream = false}, {.endOfStream = true}});

      if (isBlockedPath)
      {
        gatePtr->createdPtr->fetch_add(1, std::memory_order_relaxed);
        decoderPtr->setDestroyCounter(gatePtr->destroyedPtr);
      }

      return decoderPtr;
    };
  }

  DecoderFactoryFn makeCountingDecoderFactory(std::vector<ScriptedTrack> tracks,
                                              std::shared_ptr<DecoderLifeCounters> countersPtr)
  {
    return [tracks = std::move(tracks), countersPtr = std::move(countersPtr)](std::filesystem::path const& path, Format)
    {
      for (auto const& track : tracks)
      {
        if (track.path == path)
        {
          countersPtr->created.fetch_add(1, std::memory_order_relaxed);
          auto destroyCounterPtr = std::shared_ptr<std::atomic<std::size_t>>{countersPtr, &countersPtr->destroyed};
          auto decPtr = std::make_unique<ScriptedDecoderSession>(track.info);
          decPtr->setReadScript({{.data = track.data, .endOfStream = false}, {.endOfStream = true}});
          decPtr->setDestroyCounter(std::move(destroyCounterPtr));
          return decPtr;
        }
      }

      return std::unique_ptr<ScriptedDecoderSession>{};
    };
  }

  DecoderFactoryFn makeRegisteringDecoderFactory(
    std::vector<RegisteredTrack> tracks,
    std::shared_ptr<std::map<std::filesystem::path, ScriptedDecoderSession*>> registryPtr)
  {
    return [tracks = std::move(tracks), registryPtr = std::move(registryPtr)](std::filesystem::path const& path, Format)
    {
      for (auto const& entry : tracks)
      {
        if (entry.track.path == path)
        {
          auto decPtr = std::make_unique<ScriptedDecoderSession>(entry.track.info);
          decPtr->setReadScript({{.data = entry.track.data, .endOfStream = false}, {.endOfStream = true}});

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

  void StagedFailureGate::notifyReadEntered()
  {
    _readEntered.release();
  }

  bool StagedFailureGate::waitForRead(std::chrono::milliseconds timeout)
  {
    return _readEntered.try_acquire_for(timeout);
  }

  void StagedFailureGate::waitForRelease()
  {
    _failureRelease.acquire();
  }

  void StagedFailureGate::release()
  {
    _failureRelease.release();
  }

  StagedFailureReleaseGuard::StagedFailureReleaseGuard(StagedFailureGate& gate)
    : _gate{gate}
  {
  }

  StagedFailureReleaseGuard::~StagedFailureReleaseGuard()
  {
    if (_armed)
    {
      _gate.release();
    }
  }

  void StagedFailureReleaseGuard::release()
  {
    _gate.release();
    _armed = false;
  }

  StagedFailureDecoderSession::StagedFailureDecoderSession(StagedFailureGate* failureGate)
    : _failureGate{failureGate}
  {
  }

  StagedFailureDecoderSession::~StagedFailureDecoderSession() = default;

  Result<> StagedFailureDecoderSession::open(std::filesystem::path const& /*path*/) noexcept
  {
    return {};
  }

  void StagedFailureDecoderSession::close() noexcept
  {
  }

  void StagedFailureDecoderSession::flush() noexcept
  {
  }

  Result<> StagedFailureDecoderSession::seek(std::chrono::milliseconds /*offset*/) noexcept
  {
    return {};
  }

  Result<PcmBlock> StagedFailureDecoderSession::readNextBlock() noexcept
  {
    if (!_prerollReturned)
    {
      _prerollReturned = true;
      return PcmBlock{
        .bytes = _prerollBytes,
        .bitDepth = 16,
        .frames = kPrerollFrames,
        .firstFrameIndex = 0,
        .endOfStream = false,
      };
    }

    if (_failureGate != nullptr && !_failureReturned)
    {
      _failureGate->notifyReadEntered();
      _failureGate->waitForRelease();
      _failureReturned = true;
      return std::unexpected{Error{.code = Error::Code::IoError, .message = "gated staged decode failure"}};
    }

    return PcmBlock{.bitDepth = 16, .endOfStream = true};
  }

  DecodedStreamInfo StagedFailureDecoderSession::streamInfo() const noexcept
  {
    auto const format = makeEngineTestFormat();
    return DecodedStreamInfo{
      .sourceFormat = format,
      .outputFormat = format,
      .duration = std::chrono::seconds{3},
      .isLossy = false,
      .codec = AudioCodec::Flac,
    };
  }

  DecoderFactoryFn makeStagedFailureDecoderFactory(std::filesystem::path failingPath, StagedFailureGate& failureGate)
  {
    return [failingPath = std::move(failingPath), &failureGate](std::filesystem::path const& path, Format)
    { return std::make_unique<StagedFailureDecoderSession>(path == failingPath ? &failureGate : nullptr); };
  }

  void CallbackLatch::notify()
  {
    auto const lock = std::scoped_lock{_mutex};
    ++_count;
    _cv.notify_all();
  }

  bool CallbackLatch::waitForCount(std::size_t expected, std::chrono::milliseconds timeout)
  {
    auto lock = std::unique_lock{_mutex};
    return _cv.wait_for(lock, timeout, [this, expected] { return _count >= expected; });
  }

  std::size_t CallbackLatch::count() const
  {
    auto const lock = std::scoped_lock{_mutex};
    return _count;
  }
} // namespace ao::audio::test

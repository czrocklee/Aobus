// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "ScriptedDecoderSession.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

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
#include <semaphore>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  Device makeEngineTestDevice(std::string_view id = "test-device");
  PcmFormat makeEngineTestFormat();
  SignalFormat makeEngineTestSignalFormat();
  Engine::PlaybackItem makePlaybackItem(std::filesystem::path path);
  Engine::PlaybackItem makePlaybackItem(PlaybackInput input);
  DecoderFactoryFn makeScriptedEngineDecoderFactory(PcmFormat fmt = makeEngineTestFormat());

  struct ScriptedTrack final
  {
    std::filesystem::path path;
    DecodedStreamInfo info;
    std::vector<std::byte> data;
  };

  DecodedStreamInfo makeScriptedStreamInfo(PcmFormat format, AudioCodec codec = AudioCodec::Flac, bool isLossy = false);
  DecoderFactoryFn makePathScriptedDecoderFactory(std::vector<ScriptedTrack> tracks);

  // Tracks how many decoder sessions are live. Track preparation opens a
  // short-lived probe decoder and then the streaming decoder per track, so
  // only `live()` measured at a settled point (never mid-open) equals the
  // number of streaming sources currently alive.
  struct DecoderLifeCounters final
  {
    std::atomic<std::size_t> created{0};
    std::atomic<std::size_t> destroyed{0};

    std::size_t live() const;
  };

  struct BlockingPreparationGate final
  {
    void enterAndWait();
    bool waitForEntry(std::chrono::milliseconds timeout = std::chrono::seconds{5});

    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    std::atomic<bool> blocked{false};
    std::shared_ptr<std::atomic<std::size_t>> createdPtr = std::make_shared<std::atomic<std::size_t>>(0);
    std::shared_ptr<std::atomic<std::size_t>> destroyedPtr = std::make_shared<std::atomic<std::size_t>>(0);
  };

  DecoderFactoryFn makeBlockingPreparationDecoderFactory(std::shared_ptr<BlockingPreparationGate> gatePtr,
                                                         std::filesystem::path blockedPath);

  // Like makePathScriptedDecoderFactory, but every decoder it creates bumps the
  // shared life counters, so a test can observe that retired gapless sources
  // are reclaimed rather than accumulated across a continuous splice run.
  DecoderFactoryFn makeCountingDecoderFactory(std::vector<ScriptedTrack> tracks,
                                              std::shared_ptr<DecoderLifeCounters> countersPtr);

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
  DecoderFactoryFn makeRegisteringDecoderFactory(
    std::vector<RegisteredTrack> tracks,
    std::shared_ptr<std::map<std::filesystem::path, ScriptedDecoderSession*>> registryPtr);

  class StagedFailureGate final
  {
  public:
    void notifyReadEntered();
    bool waitForRead(std::chrono::milliseconds timeout = std::chrono::seconds{5});
    void waitForRelease();
    void release();

  private:
    std::binary_semaphore _readEntered{0};
    std::binary_semaphore _failureRelease{0};
  };

  class [[nodiscard]] StagedFailureReleaseGuard final
  {
  public:
    explicit StagedFailureReleaseGuard(StagedFailureGate& gate);
    ~StagedFailureReleaseGuard();

    void release();

    StagedFailureReleaseGuard(StagedFailureReleaseGuard const&) = delete;
    StagedFailureReleaseGuard& operator=(StagedFailureReleaseGuard const&) = delete;
    StagedFailureReleaseGuard(StagedFailureReleaseGuard&&) = delete;
    StagedFailureReleaseGuard& operator=(StagedFailureReleaseGuard&&) = delete;

  private:
    StagedFailureGate& _gate;
    bool _armed = true;
  };

  class [[nodiscard]] StagedFailureDecoderSession final : public DecoderSession
  {
  public:
    StagedFailureDecoderSession(StagedFailureGate* failureGate, std::optional<SampleEncoding> optOutputEncoding);

    void flush() noexcept override;
    Result<> seek(std::chrono::milliseconds offset) noexcept override;
    Result<PcmBlock> readNextBlock() noexcept override;
    DecodedStreamInfo streamInfo() const noexcept override;

    StagedFailureDecoderSession(StagedFailureDecoderSession const&) = delete;
    StagedFailureDecoderSession& operator=(StagedFailureDecoderSession const&) = delete;
    StagedFailureDecoderSession(StagedFailureDecoderSession&&) = delete;
    StagedFailureDecoderSession& operator=(StagedFailureDecoderSession&&) = delete;
    ~StagedFailureDecoderSession() override;

  private:
    static constexpr std::uint32_t kPrerollFrames = 25000;

    StagedFailureGate* _failureGate = nullptr;
    std::optional<SampleEncoding> _optOutputEncoding;
    std::vector<std::byte> _prerollBytes =
      std::vector<std::byte>(static_cast<std::size_t>(kPrerollFrames) * 4U, std::byte{0});
    bool _prerollReturned = false;
    bool _failureReturned = false;
  };

  DecoderFactoryFn makeStagedFailureDecoderFactory(std::filesystem::path failingPath, StagedFailureGate& failureGate);

  class CallbackLatch final
  {
  public:
    void notify();
    bool waitForCount(std::size_t expected, std::chrono::milliseconds timeout = std::chrono::seconds{1});
    std::size_t count() const;

  private:
    mutable std::mutex _mutex;
    std::condition_variable _cv;
    std::size_t _count = 0;
  };
} // namespace ao::audio::test

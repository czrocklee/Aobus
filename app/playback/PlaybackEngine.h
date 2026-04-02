// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "FfmpegDecoderSession.h"
#include "IAudioBackend.h"
#include "PcmRingBuffer.h"
#include "PlaybackTypes.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace app::playback
{

  class PlaybackEngine final
  {
  public:
    explicit PlaybackEngine(std::unique_ptr<IAudioBackend> backend);
    ~PlaybackEngine();

    void play(TrackPlaybackDescriptor descriptor);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    PlaybackSnapshot snapshot() const;

  private:
    void openTrack(TrackPlaybackDescriptor descriptor);
    void stopDecodeThread();
    void decodeLoop(std::stop_token stopToken);

    static std::size_t onReadPcm(void* userData, std::span<std::byte> output) noexcept;
    static void onUnderrun(void* userData) noexcept;
    static void onPositionAdvanced(void* userData, std::uint32_t frames) noexcept;

    std::unique_ptr<IAudioBackend> _backend;
    std::optional<FfmpegDecoderSession> _decoder;
    PcmRingBuffer _ringBuffer;
    std::jthread _decodeThread;

    mutable std::mutex _stateMutex;
    mutable std::mutex _decoderMutex;
    PlaybackSnapshot _snapshot;
    std::optional<TrackPlaybackDescriptor> _currentTrack;

    std::atomic<TransportState> _state = TransportState::Idle;
    std::atomic<std::uint32_t> _bufferedMs = 0;
    std::atomic<std::uint32_t> _underrunCount = 0;
  };

} // namespace app::playback
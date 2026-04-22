// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/PlaybackTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace app::core::playback
{

  struct AudioRenderCallbacks final
  {
    void* userData = nullptr;
    std::size_t (*readPcm)(void* userData, std::span<std::byte> output) noexcept = nullptr;
    bool (*isSourceDrained)(void* userData) noexcept = nullptr;
    void (*onUnderrun)(void* userData) noexcept = nullptr;
    void (*onPositionAdvanced)(void* userData, std::uint32_t frames) noexcept = nullptr;
    void (*onDrainComplete)(void* userData) noexcept = nullptr;
  };

  class IAudioBackend
  {
  public:
    virtual ~IAudioBackend() = default;

    virtual bool open(StreamFormat const& format, AudioRenderCallbacks callbacks) = 0;
    virtual void start() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void flush() = 0;
    virtual void drain() = 0;
    virtual void stop() = 0;
    virtual void close() = 0;
    virtual BackendKind kind() const noexcept = 0;
    virtual BackendFormatInfo formatInfo() const = 0;
    virtual std::string_view lastError() const noexcept = 0;
  };

} // namespace app::core::playback

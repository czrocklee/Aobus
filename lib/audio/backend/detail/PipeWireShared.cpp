// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Format.h>
#include <ao/audio/backend/detail/PipeWireShared.h>

extern "C"
{
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/pod.h>
}

#include <charconv>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>

namespace ao::audio::backend::detail
{
  namespace
  {
    struct PipeWireRuntimeState final
    {
      std::mutex mutex;
      std::uint32_t refCount = 0;
    };

    PipeWireRuntimeState& pipeWireRuntimeState()
    {
      static auto* const state = new PipeWireRuntimeState{};
      return *state;
    }
  } // namespace

  PipeWireEnvironmentGuard::PipeWireEnvironmentGuard()
  {
    auto& state = pipeWireRuntimeState();
    auto const lock = std::scoped_lock{state.mutex};

    if (state.refCount == 0)
    {
      ::pw_init(nullptr, nullptr);
    }

    ++state.refCount;
    _active = true;
  }

  PipeWireEnvironmentGuard::~PipeWireEnvironmentGuard() noexcept
  {
    if (!_active)
    {
      return;
    }

    auto& state = pipeWireRuntimeState();
    auto const lock = std::scoped_lock{state.mutex};
    --state.refCount;

    if (state.refCount == 0)
    {
      ::pw_deinit();
    }
  }

  std::optional<std::uint32_t> parseUintProperty(char const* value) noexcept
  {
    if (value == nullptr || *value == '\0')
    {
      return std::nullopt;
    }

    auto const text = std::string_view{value};
    auto parsed = std::uint32_t{0};
    auto const* const begin = text.data();
    auto const* const end = begin + text.size();
    auto const [ptr, ec] = std::from_chars(begin, end, parsed);

    if (ec != std::errc{} || ptr != end)
    {
      return std::nullopt;
    }

    return parsed;
  }

  std::optional<Format> parseRawStreamFormat(::spa_pod const* param) noexcept
  {
    if (param == nullptr)
    {
      return std::nullopt;
    }

    auto info = ::spa_audio_info_raw{};

    if (::spa_format_audio_raw_parse(param, &info) < 0)
    {
      return std::nullopt;
    }

    auto format = Format{};
    format.sampleRate = info.rate;
    format.channels = static_cast<std::uint8_t>(info.channels);
    format.isInterleaved = true;

    if (info.format == SPA_AUDIO_FORMAT_S16_LE || info.format == SPA_AUDIO_FORMAT_S16_BE)
    {
      format.bitDepth = 16;
      format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S24_LE || info.format == SPA_AUDIO_FORMAT_S24_BE)
    {
      format.bitDepth = 24;
      format.validBits = 24;
      format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S24_32_LE || info.format == SPA_AUDIO_FORMAT_S24_32_BE)
    {
      format.bitDepth = 32;
      format.validBits = 24;
      format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S32_LE || info.format == SPA_AUDIO_FORMAT_S32_BE)
    {
      format.bitDepth = 32;
      format.validBits = 32;
      format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_F32_LE || info.format == SPA_AUDIO_FORMAT_F32_BE)
    {
      format.bitDepth = 32;
      format.validBits = 32;
      format.isFloat = true;
    }
    else if (info.format == SPA_AUDIO_FORMAT_F64_LE || info.format == SPA_AUDIO_FORMAT_F64_BE)
    {
      format.bitDepth = 64;
      format.validBits = 64;
      format.isFloat = true;
    }
    else
    {
      return std::nullopt;
    }

    return format;
  }
} // namespace ao::audio::backend::detail

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/detail/PipeWireShared.h>

#include <ao/audio/Format.h>

extern "C"
{
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/pod.h>
}

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace ao::audio::backend::detail
{
  void ensurePipeWireInit()
  {
    struct PwInitGuard
    {
      PwInitGuard() { ::pw_init(nullptr, nullptr); }
      ~PwInitGuard() noexcept { ::pw_deinit(); }

      PwInitGuard(PwInitGuard const&) = delete;
      PwInitGuard& operator=(PwInitGuard const&) = delete;
      PwInitGuard(PwInitGuard&&) = delete;
      PwInitGuard& operator=(PwInitGuard&&) = delete;
    };

    static PwInitGuard const guard;
  }

  std::optional<std::uint32_t> parseUintProperty(char const* value) noexcept
  {
    if (value == nullptr || *value == '\0' || (std::isspace(static_cast<unsigned char>(*value)) != 0))
    {
      return std::nullopt;
    }

    char* end = nullptr; // NOLINT(misc-const-correctness)
    auto const parsed = ::strtoul(value, &end, 10);

    if (end == value || *end != '\0')
    {
      return std::nullopt;
    }

    return static_cast<std::uint32_t>(parsed);
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

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/detail/PipeWireShared.h"

extern "C"
{
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
}

namespace app::playback::detail
{

  void ensurePipeWireInit()
  {
    struct PwInitGuard
    {
      PwInitGuard() { ::pw_init(nullptr, nullptr); }
      ~PwInitGuard() { ::pw_deinit(); }
    };
    static PwInitGuard guard;
  }

  std::optional<std::uint32_t> parseUintProperty(char const* value)
  {
    if (value == nullptr || *value == '\0') return std::nullopt;
    char* end = nullptr;
    auto const parsed = ::strtoul(value, &end, 10);
    if (end == value) return std::nullopt;
    return static_cast<std::uint32_t>(parsed);
  }

  std::optional<app::core::AudioFormat> parseRawStreamFormat(::spa_pod const* param)
  {
    if (param == nullptr) return std::nullopt;
    auto info = ::spa_audio_info_raw{};
    if (::spa_format_audio_raw_parse(param, &info) < 0) return std::nullopt;

    auto format = app::core::AudioFormat{};
    format.sampleRate = info.rate;
    format.channels = static_cast<std::uint8_t>(info.channels);
    format.isInterleaved = true;

    if (info.format == SPA_AUDIO_FORMAT_S16 || info.format == SPA_AUDIO_FORMAT_S16_LE ||
        info.format == SPA_AUDIO_FORMAT_S16_BE)
    {
      format.bitDepth = 16;
      format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S24 || info.format == SPA_AUDIO_FORMAT_S24_LE ||
             info.format == SPA_AUDIO_FORMAT_S24_BE)
    {
      format.bitDepth = 24;
      format.validBits = 24;
      format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S24_32 || info.format == SPA_AUDIO_FORMAT_S24_32_LE ||
             info.format == SPA_AUDIO_FORMAT_S24_32_BE)
    {
      format.bitDepth = 32;
      format.validBits = 24;
      format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S32 || info.format == SPA_AUDIO_FORMAT_S32_LE ||
             info.format == SPA_AUDIO_FORMAT_S32_BE)
    {
      format.bitDepth = 32;
      format.validBits = 32;
      format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_F32 || info.format == SPA_AUDIO_FORMAT_F32_LE ||
             info.format == SPA_AUDIO_FORMAT_F32_BE)
    {
      format.bitDepth = 32;
      format.validBits = 32;
      format.isFloat = true;
    }
    else if (info.format == SPA_AUDIO_FORMAT_F64 || info.format == SPA_AUDIO_FORMAT_F64_LE ||
             info.format == SPA_AUDIO_FORMAT_F64_BE)
    {
      format.bitDepth = 64;
      format.validBits = 64;
      format.isFloat = true;
    }
    else
      return std::nullopt;

    return format;
  }

} // namespace app::playback::detail

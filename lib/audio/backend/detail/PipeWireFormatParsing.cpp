// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "backend/detail/PipeWireFormatParsing.h"

#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

extern "C"
{
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/pod/body.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>
#include <spa/utils/type.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>

namespace ao::audio::backend::detail
{
  namespace
  {
    std::uint32_t spaFormat(SampleEncoding const encoding) noexcept
    {
      switch (encoding)
      {
        case SampleEncoding::Signed16Le: return SPA_AUDIO_FORMAT_S16_LE;
        case SampleEncoding::Signed24PackedLe: return SPA_AUDIO_FORMAT_S24_LE;
        case SampleEncoding::Signed24In32Le: return SPA_AUDIO_FORMAT_S24_32_LE;
        case SampleEncoding::Signed32Le: return SPA_AUDIO_FORMAT_S32_LE;
        case SampleEncoding::Float32Le: return SPA_AUDIO_FORMAT_F32_LE;
        case SampleEncoding::Unknown: return SPA_AUDIO_FORMAT_UNKNOWN;
      }

      return SPA_AUDIO_FORMAT_UNKNOWN;
    }
  } // namespace

  ::spa_pod const* buildRawStreamFormatOffer(std::span<std::byte> buffer,
                                             SignalFormat const& sourceFormat,
                                             std::span<SampleEncoding const> encodings) noexcept
  {
    if (buffer.empty() || encodings.empty() || sourceFormat.sampleRate == 0U || sourceFormat.channels == 0U)
    {
      return nullptr;
    }

    for (auto const encoding : encodings)
    {
      if (spaFormat(encoding) == SPA_AUDIO_FORMAT_UNKNOWN)
      {
        return nullptr;
      }
    }

    auto builder = ::spa_pod_builder{};
    ::spa_pod_builder_init(&builder, buffer.data(), buffer.size());
    auto frame = ::spa_pod_frame{};
    ::spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    ::spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType, 0);
    ::spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_audio);
    ::spa_pod_builder_prop(&builder, SPA_FORMAT_mediaSubtype, 0);
    ::spa_pod_builder_id(&builder, SPA_MEDIA_SUBTYPE_raw);
    ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_format, 0);
    auto choiceFrame = ::spa_pod_frame{};
    ::spa_pod_builder_push_choice(&builder, &choiceFrame, SPA_CHOICE_Enum, 0);
    ::spa_pod_builder_id(&builder, spaFormat(encodings.front()));

    for (auto const encoding : encodings)
    {
      ::spa_pod_builder_id(&builder, spaFormat(encoding));
    }

    std::ignore = ::spa_pod_builder_pop(&builder, &choiceFrame);
    ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_rate, 0);
    ::spa_pod_builder_int(&builder, static_cast<std::int32_t>(sourceFormat.sampleRate));
    ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_channels, 0);
    ::spa_pod_builder_int(&builder, static_cast<std::int32_t>(sourceFormat.channels));

    if (sourceFormat.channels == 2U)
    {
      auto position = std::array<std::uint32_t, 2>{SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR};
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_position, 0);
      ::spa_pod_builder_array(&builder, sizeof(std::uint32_t), SPA_TYPE_Id, position.size(), position.data());
    }

    return static_cast<::spa_pod const*>(::spa_pod_builder_pop(&builder, &frame));
  }

  std::optional<PcmFormat> parseRawStreamFormat(::spa_pod const* param) noexcept
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

    auto format = PcmFormat{};
    format.sampleRate = info.rate;
    format.channels = static_cast<std::uint8_t>(info.channels);

    if (info.format == SPA_AUDIO_FORMAT_S16_LE)
    {
      format.encoding = SampleEncoding::Signed16Le;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S24_LE)
    {
      format.encoding = SampleEncoding::Signed24PackedLe;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S24_32_LE)
    {
      format.encoding = SampleEncoding::Signed24In32Le;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S32_LE)
    {
      format.encoding = SampleEncoding::Signed32Le;
    }
    else if (info.format == SPA_AUDIO_FORMAT_F32_LE)
    {
      format.encoding = SampleEncoding::Float32Le;
    }
    else
    {
      return std::nullopt;
    }

    return format;
  }
} // namespace ao::audio::backend::detail

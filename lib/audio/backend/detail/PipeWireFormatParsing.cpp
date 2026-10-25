// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "backend/detail/PipeWireFormatParsing.h"

#include "backend/detail/SpeakerSet.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
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
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <spa/utils/defs.h>
#include <spa/utils/type.h>
}

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

namespace ao::audio::backend::detail
{
  namespace
  {
    // WAVE assigned-bit order, not SPA enum order.
    constexpr auto kWavePositions = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL,
                                                                  SPA_AUDIO_CHANNEL_FR,
                                                                  SPA_AUDIO_CHANNEL_FC,
                                                                  SPA_AUDIO_CHANNEL_LFE,
                                                                  SPA_AUDIO_CHANNEL_RL,
                                                                  SPA_AUDIO_CHANNEL_RR,
                                                                  SPA_AUDIO_CHANNEL_FLC,
                                                                  SPA_AUDIO_CHANNEL_FRC,
                                                                  SPA_AUDIO_CHANNEL_RC,
                                                                  SPA_AUDIO_CHANNEL_SL,
                                                                  SPA_AUDIO_CHANNEL_SR,
                                                                  SPA_AUDIO_CHANNEL_TC,
                                                                  SPA_AUDIO_CHANNEL_TFL,
                                                                  SPA_AUDIO_CHANNEL_TFC,
                                                                  SPA_AUDIO_CHANNEL_TFR,
                                                                  SPA_AUDIO_CHANNEL_TRL,
                                                                  SPA_AUDIO_CHANNEL_TRC,
                                                                  SPA_AUDIO_CHANNEL_TRR});

    struct PositionSet final
    {
      std::uint32_t mask = 0;
      bool hasUnsupportedPositions = false;
    };

    std::optional<PositionSet> positionSet(RawStreamFormat const& native) noexcept
    {
      if ((native.flags & SPA_AUDIO_FLAG_UNPOSITIONED) != 0U)
      {
        return PositionSet{};
      }

      if (native.format.channels > SPA_AUDIO_MAX_CHANNELS)
      {
        return std::nullopt;
      }

      auto positions = PositionSet{};

      for (std::size_t slot = 0; slot < native.format.channels; ++slot)
      {
        auto const position = native.positions[slot];
        bool const isAux = SPA_AUDIO_CHANNEL_IS_AUX(position);
        bool const isCustom =
          position >= static_cast<std::uint32_t>(SPA_AUDIO_CHANNEL_START_Custom) && position != SPA_ID_INVALID;

        // Reserved gaps and SPA_ID_INVALID are invalid position metadata: a role
        // semantic, not a byte-extent failure. CUSTOM ids remain role constraints.
        if (position > SPA_AUDIO_CHANNEL_BRC && !isAux && !isCustom)
        {
          return std::nullopt;
        }

        // UNKNOWN is unspecified and NA is a silent placeholder, not a unique
        // speaker identity. MONO, AUX, CUSTOM, and assigned roles stay unique.
        if (position != SPA_AUDIO_CHANNEL_UNKNOWN && position != SPA_AUDIO_CHANNEL_NA)
        {
          for (std::size_t previous = 0; previous < slot; ++previous)
          {
            if (native.positions[previous] == position)
            {
              return std::nullopt;
            }
          }
        }

        auto const* const it = std::ranges::find(kWavePositions, position);

        if (it == kWavePositions.end())
        {
          // Only unspecified, MONO and AUX labels leave WAVE identity unknown.
          // Silent NA, CUSTOM, and other identified non-WAVE roles remain constraints.
          positions.hasUnsupportedPositions |=
            position != SPA_AUDIO_CHANNEL_UNKNOWN && position != SPA_AUDIO_CHANNEL_MONO && !isAux;
          continue;
        }

        positions.mask |= 1U << static_cast<std::uint32_t>(it - kWavePositions.begin());
      }

      return positions;
    }

    // Canonical storage promise only. Non-ascending WAVE slot order clears
    // format.speakerMask and leaves nativeSpeakerMask unchanged. Callers keep
    // strict versus best-effort completeness gates outside this helper.
    void clearNoncanonicalWaveSpeakerMask(RawStreamFormat& native) noexcept
    {
      std::size_t slot = 0;

      for (std::size_t bit = 0; native.format.speakerMask != 0U && bit < kWavePositions.size(); ++bit)
      {
        if ((native.nativeSpeakerMask & (1U << bit)) == 0U)
        {
          continue;
        }

        if (native.positions[slot++] != kWavePositions[bit])
        {
          native.format.speakerMask = 0;
          break;
        }
      }
    }

    // Validate property extents before using SPA's property lookup. The caller
    // owns the complete pod storage described by param->size.
    bool hasValidRawFormatStorage(::spa_pod const* param) noexcept
    {
      if (param == nullptr || param->type != SPA_TYPE_Object || param->size < sizeof(::spa_pod_object_body) ||
          param->size % 8U != 0U)
      {
        return false;
      }

      auto const bytes =
        std::span{static_cast<std::byte const*>(SPA_POD_BODY_CONST(param)), static_cast<std::size_t>(param->size)};
      auto object = ::spa_pod_object_body{};
      std::memcpy(&object, bytes.data(), sizeof(object));

      if (object.type != SPA_TYPE_OBJECT_Format)
      {
        return false;
      }

      std::size_t offset = sizeof(object);

      while (offset < bytes.size())
      {
        auto const remaining = bytes.size() - offset;

        if (remaining < sizeof(::spa_pod_prop))
        {
          return false;
        }

        auto property = ::spa_pod_prop{};
        std::memcpy(&property, bytes.data() + offset, sizeof(property));

        if (property.value.size > remaining - sizeof(property))
        {
          return false;
        }

        auto const propertySize = sizeof(property) + static_cast<std::size_t>(property.value.size);
        auto const padding = (8U - (propertySize % 8U)) % 8U;

        // Native object parsing requires padded storage, not just a value
        // reachable by direct property lookup. Bound padding by subtraction
        // before addition so the advance cannot overflow size_t either.
        if (padding > remaining - propertySize)
        {
          return false;
        }

        offset += propertySize + padding;
      }

      return true;
    }

    std::optional<std::uint32_t> fixedScalar(::spa_pod const& value, std::uint32_t const type) noexcept
    {
      auto const* data = static_cast<std::byte const*>(SPA_POD_BODY_CONST(&value));

      if (value.type == SPA_TYPE_Choice)
      {
        if (value.size < sizeof(::spa_pod_choice_body))
        {
          return std::nullopt;
        }

        auto body = ::spa_pod_choice_body{};
        std::memcpy(&body, data, sizeof(body));

        if (auto const payloadSize = value.size - sizeof(body);
            body.type != SPA_CHOICE_None || body.child.type != type || body.child.size != sizeof(std::uint32_t) ||
            payloadSize == 0U || payloadSize % sizeof(std::uint32_t) != 0U)
        {
          return std::nullopt;
        }

        data += sizeof(body);
      }
      else if (value.type != type || value.size != sizeof(std::uint32_t))
      {
        return std::nullopt;
      }

      std::uint32_t scalar = 0;
      std::memcpy(&scalar, data, sizeof(scalar));
      return scalar;
    }

    // Unlike spa_format_audio_raw_parse, this never copies a position array or
    // constrains the channel count to spa_audio_info_raw's position capacity.
    std::optional<PcmFormat> rawStreamNumerics(::spa_pod const* param) noexcept
    {
      if (!hasValidRawFormatStorage(param))
      {
        return std::nullopt;
      }

      auto const* encoding = ::spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_format);
      auto const* rate = ::spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_rate);
      auto const* channels = ::spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_channels);

      if (encoding == nullptr || rate == nullptr || channels == nullptr)
      {
        return std::nullopt;
      }

      auto const optEncoding = fixedScalar(encoding->value, SPA_TYPE_Id);
      auto const optRate = fixedScalar(rate->value, SPA_TYPE_Int);
      auto const optChannels = fixedScalar(channels->value, SPA_TYPE_Int);

      if (!optEncoding || !optRate || !optChannels || std::bit_cast<std::int32_t>(*optRate) <= 0 ||
          *optChannels == 0U || *optChannels > std::numeric_limits<std::uint8_t>::max())
      {
        return std::nullopt;
      }

      auto format = PcmFormat{.sampleRate = *optRate, .channels = static_cast<std::uint8_t>(*optChannels)};

      switch (*optEncoding)
      {
        case SPA_AUDIO_FORMAT_S16_LE: format.encoding = SampleEncoding::Signed16Le; break;
        case SPA_AUDIO_FORMAT_S24_LE: format.encoding = SampleEncoding::Signed24PackedLe; break;
        case SPA_AUDIO_FORMAT_S24_32_LE: format.encoding = SampleEncoding::Signed24In32Le; break;
        case SPA_AUDIO_FORMAT_S32_LE: format.encoding = SampleEncoding::Signed32Le; break;
        case SPA_AUDIO_FORMAT_F32_LE: format.encoding = SampleEncoding::Float32Le; break;
        default: return std::nullopt;
      }

      return format;
    }

    ::spa_pod const* fixedPositionArray(::spa_pod const& value) noexcept
    {
      auto const* array = &value;

      if (value.type == SPA_TYPE_Choice)
      {
        if (::spa_pod_is_choice(&value) == 0)
        {
          return nullptr;
        }

        auto body = ::spa_pod_choice_body{};
        std::memcpy(&body, SPA_POD_BODY_CONST(&value), sizeof(body));

        if (auto const payloadSize = value.size - sizeof(body);
            body.type != SPA_CHOICE_None || body.child.type != SPA_TYPE_Array ||
            body.child.size < sizeof(::spa_pod_array_body) || body.child.size > payloadSize ||
            payloadSize % body.child.size != 0U)
        {
          return nullptr;
        }

        // SPA_CHOICE_None makes the first value current. Fixation changes the
        // choice kind without necessarily removing retained alternative storage.
        std::uint32_t count = 0;
        std::uint32_t choice = 0;
        array = ::spa_pod_get_values(&value, &count, &choice);

        if (array == nullptr || count == 0U || choice != SPA_CHOICE_None)
        {
          return nullptr;
        }
      }

      if (array->type != SPA_TYPE_Array || array->size < sizeof(::spa_pod_array_body))
      {
        return nullptr;
      }

      auto body = ::spa_pod_array_body{};
      std::memcpy(&body, SPA_POD_BODY_CONST(array), sizeof(body));

      if (auto const payloadSize = array->size - sizeof(body); body.child.type != SPA_TYPE_Id ||
                                                               body.child.size != sizeof(std::uint32_t) ||
                                                               payloadSize % sizeof(std::uint32_t) != 0U)
      {
        return nullptr;
      }

      return array;
    }

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

    void completeRawStreamPermutation(RawStreamFormat const& native,
                                      std::uint32_t const sourceMask,
                                      RawStreamDelivery& delivery) noexcept
    {
      auto usedSources = std::array<bool, SPA_AUDIO_MAX_CHANNELS>{};
      std::ranges::fill(delivery.sourceSlots, std::numeric_limits<std::uint8_t>::max());

      // After role validation, assign observed destinations to their canonical
      // source slots. Unknown destinations impose no role identity; unused
      // sources complete the bijection without claiming unknown native roles.
      for (std::size_t slot = 0; slot < delivery.clientFormat.channels; ++slot)
      {
        std::uint8_t canonicalSlot = 0;

        for (std::size_t bit = 0; bit < kWavePositions.size(); ++bit)
        {
          if ((sourceMask & (1U << bit)) == 0U)
          {
            continue;
          }

          if (native.positions[slot] == kWavePositions[bit])
          {
            delivery.sourceSlots[slot] = canonicalSlot;
            usedSources[canonicalSlot] = true;
            break;
          }

          ++canonicalSlot;
        }
      }

      for (std::size_t slot = 0; slot < delivery.clientFormat.channels; ++slot)
      {
        if (delivery.sourceSlots[slot] == std::numeric_limits<std::uint8_t>::max() && !usedSources[slot])
        {
          delivery.sourceSlots[slot] = static_cast<std::uint8_t>(slot);
          usedSources[slot] = true;
        }
      }

      for (std::size_t slot = 0; slot < delivery.clientFormat.channels; ++slot)
      {
        if (delivery.sourceSlots[slot] == std::numeric_limits<std::uint8_t>::max())
        {
          auto* const it = std::ranges::find(usedSources, false);
          auto const sourceSlot = static_cast<std::uint8_t>(it - usedSources.begin());
          delivery.sourceSlots[slot] = sourceSlot;
          usedSources[sourceSlot] = true;
        }

        delivery.needsPermutation |= delivery.sourceSlots[slot] != slot;
      }
    }
  } // namespace

  ::spa_pod const* buildRawStreamFormatOffer(std::span<std::byte> buffer,
                                             SignalFormat const& sourceFormat,
                                             std::span<SampleEncoding const> encodings) noexcept
  {
    if (buffer.empty() || encodings.empty() || sourceFormat.sampleRate == 0U || sourceFormat.channels == 0U ||
        sourceFormat.sampleRate > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        !hasSupportedSourceSpeakerSet(sourceFormat))
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
      auto position = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});

      if (sourceFormat.speakerMask != 0U)
      {
        std::size_t slot = 0;

        for (std::size_t bit = 0; bit < kWavePositions.size(); ++bit)
        {
          if ((sourceFormat.speakerMask & (1U << bit)) != 0U)
          {
            position[slot++] = kWavePositions[bit];
          }
        }
      }

      ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_position, 0);
      ::spa_pod_builder_array(&builder, sizeof(std::uint32_t), SPA_TYPE_Id, position.size(), position.data());
    }

    return static_cast<::spa_pod const*>(::spa_pod_builder_pop(&builder, &frame));
  }

  std::optional<RawStreamFormat> parseRawStreamObservation(::spa_pod const* param) noexcept
  {
    auto const optFormat = rawStreamNumerics(param);

    if (!optFormat)
    {
      return std::nullopt;
    }

    ::spa_pod const* positions = nullptr;

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_position); prop != nullptr)
    {
      // Bound the active array before the native parser can copy its values.
      positions = fixedPositionArray(prop->value);

      if (positions == nullptr)
      {
        return std::nullopt;
      }

      std::uint32_t count = 0;
      std::uint32_t size = 0;
      std::uint32_t type = 0;
      auto const* values = ::spa_pod_get_array_full(positions, &count, &size, &type);

      if (values == nullptr || count != optFormat->channels || count > SPA_AUDIO_MAX_CHANNELS ||
          size != sizeof(std::uint32_t) || type != SPA_TYPE_Id)
      {
        return std::nullopt;
      }
    }

    auto info = ::spa_audio_info_raw{};

    if (::spa_format_audio_raw_parse(param, &info) < 0)
    {
      return std::nullopt;
    }

    auto native = RawStreamFormat{.format = *optFormat, .flags = info.flags};
    static_assert(std::size(info.position) == std::tuple_size_v<decltype(native.positions)>);
    std::ranges::copy(info.position, native.positions.begin());
    auto const optPositions = positionSet(native);

    if (!optPositions)
    {
      return std::nullopt;
    }

    native.nativeSpeakerMask = optPositions->mask;
    native.hasUnsupportedPositions = optPositions->hasUnsupportedPositions;
    native.format.speakerMask =
      std::cmp_equal(std::popcount(optPositions->mask), info.channels) ? optPositions->mask : 0U;
    clearNoncanonicalWaveSpeakerMask(native);
    return native;
  }

  std::optional<RawStreamFormat> parseRawMonitorObservation(::spa_pod const* param) noexcept
  {
    auto const optFormat = rawStreamNumerics(param);

    if (!optFormat)
    {
      return std::nullopt;
    }

    auto native = RawStreamFormat{.format = *optFormat};
    auto const* property = ::spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_position);

    if (property == nullptr)
    {
      native.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
      return native;
    }

    auto const* positions = fixedPositionArray(property->value);

    if (positions == nullptr)
    {
      return std::nullopt;
    }

    std::uint32_t count = 0;
    std::uint32_t size = 0;
    std::uint32_t type = 0;
    auto const* values = static_cast<std::byte const*>(::spa_pod_get_array_full(positions, &count, &size, &type));

    if (values == nullptr || size != sizeof(std::uint32_t) || type != SPA_TYPE_Id)
    {
      return std::nullopt;
    }

    if (count != native.format.channels)
    {
      // A mismatched table does not establish this geometry's active positions.
      // Preserve numerics without treating its prefix as applied role constraints.
      native.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
      native.hasUnsupportedPositions = true;
      return native;
    }

    native.hasUnsupportedPositions = count > native.positions.size();
    auto const activeCount = static_cast<std::size_t>(count);

    for (std::size_t slot = 0; slot < activeCount; ++slot)
    {
      std::uint32_t position = 0;
      std::memcpy(&position, values + (slot * sizeof(position)), sizeof(position));

      if (slot < native.positions.size())
      {
        native.positions[slot] = position;
      }

      if (position != SPA_AUDIO_CHANNEL_UNKNOWN)
      {
        for (std::size_t previous = 0; previous < slot; ++previous)
        {
          std::uint32_t previousPosition = 0;
          std::memcpy(&previousPosition, values + (previous * sizeof(previousPosition)), sizeof(previousPosition));
          native.hasUnsupportedPositions |= previousPosition == position;
        }
      }

      auto const* const it = std::ranges::find(kWavePositions, position);

      if (it != kWavePositions.end())
      {
        native.nativeSpeakerMask |= 1U << static_cast<std::uint32_t>(it - kWavePositions.begin());
      }
      else
      {
        native.hasUnsupportedPositions |= position != SPA_AUDIO_CHANNEL_UNKNOWN && position != SPA_AUDIO_CHANNEL_MONO &&
                                          !SPA_AUDIO_CHANNEL_IS_AUX(position);
      }
    }

    // A best-effort role subset never asserts canonical storage. Only a complete,
    // representable, non-repeated native sequence can make that promise.
    if (!native.hasUnsupportedPositions &&
        std::cmp_equal(std::popcount(native.nativeSpeakerMask), native.format.channels))
    {
      native.format.speakerMask = native.nativeSpeakerMask;
      clearNoncanonicalWaveSpeakerMask(native);
    }

    return native;
  }

  std::expected<RawStreamDelivery, BackendOpenFailure> rawStreamDelivery(
    SignalFormat const& sourceFormat,
    RawStreamFormat const& native,
    std::optional<RawStreamFormat> const& optPrevious)
  {
    // Source-zero mono/multichannel never installs a permutation. Its native
    // role facts may refresh without changing immutable client byte transport.
    // Two-channel offers stay exact; only zero-mask sources default to FL/FR.
    bool const hasLegacyIdentityTransport = sourceFormat.speakerMask == 0U && sourceFormat.channels != 2U;

    if (optPrevious && *optPrevious != native &&
        (!hasLegacyIdentityTransport || optPrevious->format.sampleRate != native.format.sampleRate ||
         optPrevious->format.channels != native.format.channels ||
         optPrevious->format.encoding != native.format.encoding))
    {
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::FormatRejected, "PipeWire changed PCM mode after stream activation").error()}};
    }

    if (sourceFormat.sampleRate != native.format.sampleRate || sourceFormat.channels != native.format.channels ||
        sourceFormat.channels == 0U || !hasSupportedSourceSpeakerSet(sourceFormat) ||
        native.format.encoding == SampleEncoding::Unknown)
    {
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::FormatRejected, "PipeWire negotiated invalid PCM geometry").error()}};
    }

    auto const optPositions = positionSet(native);

    if (!optPositions)
    {
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::FormatRejected, "PipeWire negotiated malformed channel positions").error()}};
    }

    bool const exactOffer = sourceFormat.channels == 2U;
    auto const offeredMask = exactOffer && sourceFormat.speakerMask == 0U ? 0x3U : sourceFormat.speakerMask;

    // An unpositioned source imposes no role constraint in the legacy
    // mono/multichannel offer. Retain unsupported observations as uncertainty,
    // not a new source-zero playback ban; exact two-channel offers stay strict.
    if ((optPositions->hasUnsupportedPositions && offeredMask != 0U) ||
        (exactOffer && optPositions->mask != offeredMask) ||
        (offeredMask != 0U && (optPositions->mask & ~offeredMask) != 0U))
    {
      return std::unexpected{BackendOpenFailure{
        .error =
          makeError(Error::Code::FormatRejected, "PipeWire client channel positions conflict with the source").error(),
        .cause =
          exactOffer ? BackendOpenFailure::Cause::OutputFailure : BackendOpenFailure::Cause::TrackRouteIncompatible}};
    }

    auto delivery = RawStreamDelivery{.clientFormat = pcmFormat(sourceFormat, native.format.encoding)};
    delivery.channelDelivery.nativeSpeakerMask = optPositions->mask;
    // Observed native evidence only. Source identity, canonical client order,
    // and zero-mask non-stereo identity transport do not set or clear this flag.
    delivery.channelDelivery.isMapped =
      !optPositions->hasUnsupportedPositions && isCompleteWaveSpeakerMask(optPositions->mask, native.format.channels);

    if (optPositions->mask == 0U || offeredMask == 0U)
    {
      return delivery;
    }

    completeRawStreamPermutation(native, offeredMask, delivery);
    return delivery;
  }

  void applyRawStreamDelivery(std::span<std::byte> bytes, RawStreamDelivery const& delivery) noexcept
  {
    if (!delivery.needsPermutation)
    {
      return;
    }

    auto const stride = frameBytes(delivery.clientFormat);
    auto const sampleBytes = bytesPerSample(delivery.clientFormat.encoding);

    if (stride == 0U || stride > SPA_AUDIO_MAX_CHANNELS * 4U)
    {
      return;
    }

    auto frame = std::array<std::byte, SPA_AUDIO_MAX_CHANNELS * 4U>{};

    for (std::size_t offset = 0; offset + stride <= bytes.size(); offset += stride)
    {
      std::ranges::copy(bytes.subspan(offset, stride), frame.begin());

      for (std::size_t slot = 0; slot < delivery.clientFormat.channels; ++slot)
      {
        auto const sourceOffset = static_cast<std::size_t>(delivery.sourceSlots[slot]) * sampleBytes;
        std::ranges::copy(std::span{frame}.subspan(sourceOffset, sampleBytes),
                          bytes.subspan(offset + (slot * sampleBytes), sampleBytes).begin());
      }
    }
  }
} // namespace ao::audio::backend::detail

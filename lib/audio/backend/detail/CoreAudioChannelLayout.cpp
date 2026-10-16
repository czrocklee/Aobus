// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioChannelLayout.h"

#include "CoreAudioError.h"
#include "CoreAudioFormat.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>

#include <AudioToolbox/AUComponent.h>
#include <AudioToolbox/AudioFormat.h>
#include <AudioToolbox/AudioUnitProperties.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <MacTypes.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail
{
  namespace
  {
    constexpr std::size_t kLayoutHeaderBytes = offsetof(::AudioChannelLayout, mChannelDescriptions);
    constexpr ::UInt32 kMaximumPropertyBytes = 65536U;
    constexpr ::UInt32 kMaximumChannelDescriptions = 256U;
    constexpr std::uint32_t kSupportedWaveRoles = 0x3FFFFU;

    enum class WaveRole : std::uint32_t
    {
      FrontLeft = 0x1U,
      FrontRight = 0x2U,
      FrontCenter = 0x4U,
      LowFrequency = 0x8U,
      BackLeft = 0x10U,
      BackRight = 0x20U,
      FrontLeftOfCenter = 0x40U,
      FrontRightOfCenter = 0x80U,
      BackCenter = 0x100U,
      SideLeft = 0x200U,
      SideRight = 0x400U,
      TopCenter = 0x800U,
      TopFrontLeft = 0x1000U,
      TopFrontCenter = 0x2000U,
      TopFrontRight = 0x4000U,
      TopBackLeft = 0x8000U,
      TopBackCenter = 0x10000U,
      TopBackRight = 0x20000U,
    };

    BackendOpenFailure observationFailure(std::string message)
    {
      return {.error = makeError(Error::Code::InitFailed, std::move(message)).error(),
              .cause = BackendOpenFailure::Cause::OutputFailure};
    }

    BackendOpenFailure roleFailure()
    {
      return {.error = makeError(Error::Code::FormatRejected,
                                 "Core Audio observed channel roles cannot carry the canonical source losslessly")
                         .error(),
              .cause = BackendOpenFailure::Cause::TrackRouteIncompatible};
    }

    bool isUnavailable(::OSStatus const status, CoreAudioChannelProperty const property) noexcept
    {
      if (property == CoreAudioChannelProperty::DeviceLayout)
      {
        // Device preferences are optional, unlike actual AU bus/map readbacks.
        // A failed preference query cannot veto transport, but device loss can.
        return status != ::noErr && !isCoreAudioDeviceLossStatus(status);
      }

      return status == ::kAudioUnitErr_InvalidProperty || status == ::kAudioUnitErr_PropertyNotInUse;
    }

    std::string_view channelPropertyName(CoreAudioChannelProperty const property) noexcept
    {
      switch (property)
      {
        case CoreAudioChannelProperty::ClientLayout: return "Core Audio client channel layout";
        case CoreAudioChannelProperty::OutputLayout: return "Core Audio output channel layout";
        case CoreAudioChannelProperty::DeviceLayout: return "Core Audio preferred device channel layout";
        case CoreAudioChannelProperty::OutputMap: return "Core Audio output ordinal map";
      }

      return "Core Audio channel observation";
    }

    std::expected<std::optional<std::vector<std::byte>>, BackendOpenFailure> readProperty(
      CoreAudioChannelOperations const& operations,
      CoreAudioChannelProperty const property)
    {
      ::UInt32 size = 0U;
      auto status = operations.size(property, size);

      if (isUnavailable(status, property))
      {
        return std::nullopt;
      }

      if (status != ::noErr)
      {
        return std::unexpected{BackendOpenFailure{
          .error =
            makeCoreAudioError(status, std::format("size {}", channelPropertyName(property)), Error::Code::InitFailed)
              .error(),
          .cause = BackendOpenFailure::Cause::OutputFailure}};
      }

      if (size == 0U || size > kMaximumPropertyBytes)
      {
        if (property == CoreAudioChannelProperty::DeviceLayout)
        {
          return std::nullopt;
        }

        return std::unexpected{observationFailure("Core Audio returned an invalid channel property size")};
      }

      auto bytes = std::vector<std::byte>(size);
      auto readSize = size;
      status = operations.read(property, readSize, bytes.data());

      if (isUnavailable(status, property))
      {
        return std::nullopt;
      }

      if (status != ::noErr)
      {
        return std::unexpected{BackendOpenFailure{
          .error =
            makeCoreAudioError(status, std::format("read {}", channelPropertyName(property)), Error::Code::InitFailed)
              .error(),
          .cause = BackendOpenFailure::Cause::OutputFailure}};
      }

      if (readSize != size)
      {
        if (property == CoreAudioChannelProperty::DeviceLayout)
        {
          return std::nullopt;
        }

        return std::unexpected{observationFailure("Core Audio channel observation changed size during read")};
      }

      return std::optional{std::move(bytes)};
    }

    std::expected<std::vector<::AudioChannelLabel>, BackendOpenFailure> layoutLabels(std::span<std::byte const> bytes)
    {
      if (bytes.size() < kLayoutHeaderBytes)
      {
        return std::unexpected{observationFailure("Core Audio returned a truncated channel layout")};
      }

      auto layout = ::AudioChannelLayout{};
      std::memcpy(&layout, bytes.data(), kLayoutHeaderBytes);
      auto const requestedTag = layout.mChannelLayoutTag;
      auto expanded = std::vector<std::byte>{};

      if (layout.mChannelLayoutTag != ::kAudioChannelLayoutTag_UseChannelDescriptions)
      {
        auto const bitmap = layout.mChannelLayoutTag == ::kAudioChannelLayoutTag_UseChannelBitmap;
        auto const property =
          bitmap ? ::kAudioFormatProperty_ChannelLayoutForBitmap : ::kAudioFormatProperty_ChannelLayoutForTag;
        auto const specifier = bitmap ? layout.mChannelBitmap : layout.mChannelLayoutTag;
        ::UInt32 expandedSize = 0U;
        auto status = ::AudioFormatGetPropertyInfo(property, sizeof(specifier), &specifier, &expandedSize);

        if (status != ::noErr)
        {
          return std::unexpected{BackendOpenFailure{
            .error =
              makeCoreAudioError(status, "size expanded Core Audio channel layout", Error::Code::InitFailed).error(),
            .cause = BackendOpenFailure::Cause::OutputFailure}};
        }

        if (expandedSize < kLayoutHeaderBytes || expandedSize > kMaximumPropertyBytes)
        {
          return std::unexpected{observationFailure("Core Audio returned an unexpandable channel layout")};
        }

        expanded.resize(expandedSize);
        auto const allocatedSize = expandedSize;
        status = ::AudioFormatGetProperty(property, sizeof(specifier), &specifier, &expandedSize, expanded.data());

        if (status != ::noErr)
        {
          return std::unexpected{BackendOpenFailure{
            .error = makeCoreAudioError(status, "expand Core Audio channel layout", Error::Code::InitFailed).error(),
            .cause = BackendOpenFailure::Cause::OutputFailure}};
        }

        if (expandedSize != allocatedSize)
        {
          return std::unexpected{observationFailure(std::format(
            "Core Audio channel layout expansion changed size: requestedTag={} allocatedBytes={} returnedBytes={}",
            requestedTag,
            allocatedSize,
            expandedSize))};
        }

        bytes = expanded;
        std::memcpy(&layout, bytes.data(), kLayoutHeaderBytes);
      }

      // AudioFormat may retain the queried standard tag while materializing
      // its descriptions. Only an actual expansion can admit that form.
      auto const hasDescriptionTag = layout.mChannelLayoutTag == ::kAudioChannelLayoutTag_UseChannelDescriptions ||
                                     (!expanded.empty() && layout.mChannelLayoutTag == requestedTag);

      if (!hasDescriptionTag || layout.mNumberChannelDescriptions == 0U ||
          layout.mNumberChannelDescriptions > kMaximumChannelDescriptions ||
          bytes.size() != kLayoutHeaderBytes + (layout.mNumberChannelDescriptions * sizeof(::AudioChannelDescription)))
      {
        return std::unexpected{observationFailure(
          std::format("Core Audio returned malformed channel descriptions: requestedTag={} returnedTag={} bitmap={} "
                      "descriptionCount={} bytes={} headerBytes={} descriptionBytes={}",
                      requestedTag,
                      layout.mChannelLayoutTag,
                      layout.mChannelBitmap,
                      layout.mNumberChannelDescriptions,
                      bytes.size(),
                      kLayoutHeaderBytes,
                      sizeof(::AudioChannelDescription)))};
      }

      auto labels = std::vector<::AudioChannelLabel>{};
      labels.reserve(layout.mNumberChannelDescriptions);

      for (::UInt32 index = 0U; index < layout.mNumberChannelDescriptions; ++index)
      {
        auto description = ::AudioChannelDescription{};
        std::memcpy(
          &description, bytes.data() + kLayoutHeaderBytes + (index * sizeof(description)), sizeof(description));
        labels.push_back(description.mChannelLabel);
      }

      return labels;
    }

    // This explicit bounded vocabulary follows the SDK's WAVE labels 1--18,
    // not a numeric cast of native bitmaps or tags. Mono is deliberately absent.
    std::uint32_t waveRole(::AudioChannelLabel const label) noexcept
    {
      switch (label)
      {
        case ::kAudioChannelLabel_Left: return std::to_underlying(WaveRole::FrontLeft);
        case ::kAudioChannelLabel_Right: return std::to_underlying(WaveRole::FrontRight);
        case ::kAudioChannelLabel_Center: return std::to_underlying(WaveRole::FrontCenter);
        case ::kAudioChannelLabel_LFEScreen: return std::to_underlying(WaveRole::LowFrequency);
        case ::kAudioChannelLabel_LeftSurround: return std::to_underlying(WaveRole::BackLeft);
        case ::kAudioChannelLabel_RightSurround: return std::to_underlying(WaveRole::BackRight);
        case ::kAudioChannelLabel_LeftCenter: return std::to_underlying(WaveRole::FrontLeftOfCenter);
        case ::kAudioChannelLabel_RightCenter: return std::to_underlying(WaveRole::FrontRightOfCenter);
        case ::kAudioChannelLabel_CenterSurround: return std::to_underlying(WaveRole::BackCenter);
        case ::kAudioChannelLabel_LeftSurroundDirect: return std::to_underlying(WaveRole::SideLeft);
        case ::kAudioChannelLabel_RightSurroundDirect: return std::to_underlying(WaveRole::SideRight);
        case ::kAudioChannelLabel_TopCenterSurround: return std::to_underlying(WaveRole::TopCenter);
        case ::kAudioChannelLabel_VerticalHeightLeft: return std::to_underlying(WaveRole::TopFrontLeft);
        case ::kAudioChannelLabel_VerticalHeightCenter: return std::to_underlying(WaveRole::TopFrontCenter);
        case ::kAudioChannelLabel_VerticalHeightRight: return std::to_underlying(WaveRole::TopFrontRight);
        case ::kAudioChannelLabel_TopBackLeft: return std::to_underlying(WaveRole::TopBackLeft);
        case ::kAudioChannelLabel_TopBackCenter: return std::to_underlying(WaveRole::TopBackCenter);
        case ::kAudioChannelLabel_TopBackRight: return std::to_underlying(WaveRole::TopBackRight);
        default: return 0U;
      }
    }

    std::uint32_t completeRoleMask(std::span<std::uint32_t const> const roles) noexcept
    {
      std::uint32_t mask = 0U;

      for (auto const role : roles)
      {
        if (role == 0U || (mask & role) != 0U)
        {
          return 0U;
        }

        mask |= role;
      }

      return mask;
    }

    std::vector<std::uint32_t> observedRoles(std::optional<std::vector<::AudioChannelLabel>> const& optLabels,
                                             std::size_t const channels)
    {
      auto roles = std::vector<std::uint32_t>(channels, 0U);

      if (optLabels && optLabels->size() == channels)
      {
        for (std::size_t slot = 0U; slot < channels; ++slot)
        {
          roles[slot] = waveRole((*optLabels)[slot]);
        }
      }

      return roles;
    }

    void projectNativeChannels(CoreAudioChannelPlan& plan,
                               PcmFormat const& client,
                               std::uint32_t const outputChannels,
                               CoreAudioChannelObservation const& observation)
    {
      auto clientRoles = observedRoles(observation.optClientLabels, client.channels);
      auto outputRoles = observedRoles(observation.optOutputLabels, outputChannels);

      // The device preference is applicable only to this actual output bus count.
      // Never truncate a larger device layout to manufacture client positions.
      if (!observation.optOutputLabels)
      {
        outputRoles = observedRoles(observation.optDeviceLabels, outputChannels);
      }

      plan.outputDelivery = {.mapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified,
                             .nativeSpeakerMask = completeRoleMask(outputRoles),
                             .hasSharedConversion = plan.delivery.hasSharedConversion};

      // A read bijective destination->source map can independently locate opaque
      // client slots. Neither the source request nor channel count supplies roles.
      if (completeRoleMask(clientRoles) == 0U && !plan.delivery.hasSharedConversion && observation.optOutputMap &&
          plan.outputDelivery.nativeSpeakerMask != 0U)
      {
        for (std::size_t destination = 0U; destination < outputRoles.size(); ++destination)
        {
          auto const source = static_cast<std::size_t>((*observation.optOutputMap)[destination]);

          if (clientRoles[source] != 0U && clientRoles[source] != outputRoles[destination])
          {
            plan.nativeClientFormat = client;
            plan.nativeClientFormat.speakerMask = 0U;
            return;
          }

          clientRoles[source] = outputRoles[destination];
        }
      }

      plan.delivery.nativeSpeakerMask = completeRoleMask(clientRoles);
      plan.nativeClientFormat = client;
      // A mask on PCM storage promises ascending canonical slot order. The
      // independently observed position SET remains useful even in another order.
      plan.nativeClientFormat.speakerMask = std::ranges::is_sorted(clientRoles) ? plan.delivery.nativeSpeakerMask : 0U;
    }

    bool isOpaqueRole(::AudioChannelLabel const label) noexcept
    {
      // Mono retains its native meaning; it neither proves Center nor a
      // contradictory spatial position. This bridge supplies no gain inference.
      return label == ::kAudioChannelLabel_Mono || label == ::kAudioChannelLabel_Unknown ||
             label == ::kAudioChannelLabel_Unused || label == ::kAudioChannelLabel_UseCoordinates ||
             label == ::kAudioChannelLabel_Discrete ||
             (label >= ::kAudioChannelLabel_Discrete_0 && label <= ::kAudioChannelLabel_Discrete_65535);
    }

    bool hasValidMapSources(std::span<::SInt32 const> const map, std::uint32_t const clientChannels) noexcept
    {
      return std::ranges::all_of(
        map, [clientChannels](auto const slot) { return slot >= -1 && std::cmp_less(slot, clientChannels); });
    }

    std::expected<std::vector<::SInt32>, BackendOpenFailure> readOutputMap(
      std::span<std::byte const> const bytes,
      std::uint32_t const clientChannels,
      std::optional<std::uint32_t> const optOutputChannels)
    {
      if (bytes.empty() || bytes.size() % sizeof(::SInt32) != 0U ||
          (optOutputChannels && bytes.size() != *optOutputChannels * sizeof(::SInt32)))
      {
        return std::unexpected{
          observationFailure("Core Audio returned a channel map with the wrong destination count")};
      }

      auto map = std::vector<::SInt32>(bytes.size() / sizeof(::SInt32));
      std::memcpy(map.data(), bytes.data(), bytes.size());

      if (!hasValidMapSources(map, clientChannels))
      {
        return std::unexpected{observationFailure("Core Audio returned an out-of-range channel map source")};
      }

      return map;
    }

    std::expected<std::vector<::AudioChannelLabel>, BackendOpenFailure> readObservedLayout(
      std::span<std::byte const> const bytes,
      CoreAudioChannelProperty const property,
      std::uint32_t const clientChannels,
      std::optional<std::uint32_t> const optOutputChannels)
    {
      auto labelsRes = layoutLabels(bytes);

      if (!labelsRes)
      {
        return std::unexpected{labelsRes.error()};
      }

      for (std::size_t slot = 0U; slot < labelsRes->size(); ++slot)
      {
        // Repeated opaque labels, including coordinates, can describe distinct slots.
        if (auto const label = (*labelsRes)[slot];
            !isOpaqueRole(label) &&
            std::ranges::contains(std::span<::AudioChannelLabel const>{*labelsRes}.first(slot), label))
        {
          return std::unexpected{observationFailure("Core Audio returned duplicate identified channel roles")};
        }
      }

      if ((property == CoreAudioChannelProperty::ClientLayout && labelsRes->size() != clientChannels) ||
          (property == CoreAudioChannelProperty::OutputLayout && optOutputChannels &&
           labelsRes->size() != *optOutputChannels))
      {
        return std::unexpected{observationFailure("Core Audio layout does not match its configured bus count")};
      }

      return labelsRes;
    }

    std::expected<std::optional<std::vector<::AudioChannelLabel>>, BackendOpenFailure> readLayoutProperty(
      CoreAudioChannelOperations const& operations,
      CoreAudioChannelProperty const property,
      std::uint32_t const clientChannels,
      std::optional<std::uint32_t> const optOutputChannels)
    {
      auto const bytesRes = readProperty(operations, property);

      if (!bytesRes)
      {
        return std::unexpected{bytesRes.error()};
      }

      if (!*bytesRes)
      {
        return std::nullopt;
      }

      auto labelsRes = readObservedLayout(**bytesRes, property, clientChannels, optOutputChannels);

      if (!labelsRes)
      {
        // Malformed preferences provide no usable fallback. Actual AU layouts
        // and maps remain strict storage/readback dependencies.
        if (property == CoreAudioChannelProperty::DeviceLayout)
        {
          return std::nullopt;
        }

        return std::unexpected{labelsRes.error()};
      }

      return std::optional{std::move(*labelsRes)};
    }

    std::expected<CoreAudioChannelObservation, BackendOpenFailure> observeChannels(
      CoreAudioChannelOperations const& operations,
      std::uint32_t const clientChannels,
      std::optional<std::uint32_t> optOutputChannels,
      bool const shouldReadPreference)
    {
      if (!operations.size || !operations.read || clientChannels == 0U ||
          (optOutputChannels && *optOutputChannels == 0U))
      {
        return std::unexpected{
          observationFailure("Core Audio channel observation requires valid buses and operations")};
      }

      auto observation = CoreAudioChannelObservation{};

      for (auto const property : {CoreAudioChannelProperty::ClientLayout,
                                  CoreAudioChannelProperty::OutputLayout,
                                  CoreAudioChannelProperty::DeviceLayout,
                                  CoreAudioChannelProperty::OutputMap})
      {
        // Current AU output labels are authoritative; preferences never prove
        // actual bus geometry and are dispensable when that authority exists.
        if (property == CoreAudioChannelProperty::DeviceLayout &&
            (!shouldReadPreference || observation.optOutputLabels))
        {
          continue;
        }

        if (property == CoreAudioChannelProperty::OutputMap)
        {
          auto const bytesRes = readProperty(operations, property);

          if (!bytesRes)
          {
            return std::unexpected{bytesRes.error()};
          }

          if (*bytesRes)
          {
            auto mapRes = readOutputMap(**bytesRes, clientChannels, optOutputChannels);

            if (!mapRes)
            {
              return std::unexpected{mapRes.error()};
            }

            observation.optOutputMap = std::move(*mapRes);
          }

          continue;
        }

        auto labelsRes = readLayoutProperty(operations, property, clientChannels, optOutputChannels);

        if (!labelsRes)
        {
          return std::unexpected{labelsRes.error()};
        }

        if (property == CoreAudioChannelProperty::ClientLayout)
        {
          observation.optClientLabels = std::move(*labelsRes);
        }
        else if (property == CoreAudioChannelProperty::OutputLayout)
        {
          observation.optOutputLabels = std::move(*labelsRes);

          if (observation.optOutputLabels)
          {
            optOutputChannels = static_cast<std::uint32_t>(observation.optOutputLabels->size());
          }
        }
        else
        {
          observation.optDeviceLabels = std::move(*labelsRes);
        }
      }

      return observation;
    }

    std::expected<std::optional<::AudioStreamBasicDescription>, BackendOpenFailure> readDownstreamFormat(
      CoreAudioChannelOperations const& operations)
    {
      auto output = ::AudioStreamBasicDescription{};

      if (auto const status = operations.readOutputFormat(output); status != ::noErr)
      {
        if (!isCoreAudioDeviceLossStatus(status))
        {
          return std::nullopt;
        }

        return std::unexpected{BackendOpenFailure{
          .error = makeCoreAudioError(status, "read Core Audio output format", Error::Code::DeviceNotFound).error(),
          .cause = BackendOpenFailure::Cause::OutputFailure}};
      }

      if (!coreAudioSignalFormat(output))
      {
        return std::unexpected{observationFailure("Core Audio returned an invalid downstream format")};
      }

      return std::optional{output};
    }

    bool hasChannelConversion(std::uint32_t const clientChannels,
                              std::uint32_t const outputChannels,
                              std::optional<std::vector<::SInt32>> const& optOutputMap)
    {
      if (clientChannels != outputChannels)
      {
        return true;
      }

      if (!optOutputMap)
      {
        return false;
      }

      auto uses = std::vector<std::uint32_t>(clientChannels, 0U);

      for (auto const source : *optOutputMap)
      {
        if (source >= 0)
        {
          ++uses[static_cast<std::size_t>(source)];
        }
      }

      return std::ranges::any_of(uses, [](auto const count) { return count != 1U; });
    }

    bool tryConstrainSource(std::optional<std::uint32_t>& optRequiredSource,
                            ::AudioChannelLabel const label,
                            std::uint32_t const speakerMask)
    {
      // Coordinates identify a spatial position, but this bounded bridge does
      // not map that vocabulary. They are not genuinely unpositioned slots.
      if (label == ::kAudioChannelLabel_UseCoordinates)
      {
        return false;
      }

      if (isOpaqueRole(label))
      {
        return true;
      }

      auto const role = waveRole(label);

      if (role == 0U || (speakerMask & role) == 0U)
      {
        return false;
      }

      auto const source = static_cast<std::uint32_t>(std::popcount(speakerMask & (role - 1U)));

      if (optRequiredSource && *optRequiredSource != source)
      {
        return false;
      }

      optRequiredSource = source;
      return true;
    }

    std::expected<std::vector<std::optional<std::uint32_t>>, BackendOpenFailure> requiredChannelSources(
      PcmFormat const& client,
      std::uint32_t const outputChannels,
      CoreAudioChannelObservation const& observation,
      bool const hasSharedConversion)
    {
      auto requiredSources = std::vector<std::optional<std::uint32_t>>(client.channels);

      if (observation.optClientLabels)
      {
        for (std::size_t slot = 0U; slot < client.channels; ++slot)
        {
          if (!tryConstrainSource(requiredSources[slot], (*observation.optClientLabels)[slot], client.speakerMask))
          {
            return std::unexpected{roleFailure()};
          }
        }
      }

      // TN2091 specifies the default ordinal relationship for a fresh output
      // unit. Bound that fallback to equal-sized buses; larger device preferences
      // alone do not describe a smaller client. A read map supplies explicit
      // destination->source correspondence even when counts differ.
      if (hasSharedConversion || (!observation.optOutputMap && outputChannels != client.channels))
      {
        return requiredSources;
      }

      // The current AU output layout is authoritative. The device preference
      // supplies constraints only when that current layout is unavailable.
      auto const& optOutput = observation.optOutputLabels ? observation.optOutputLabels : observation.optDeviceLabels;

      if (!optOutput || optOutput->size() != outputChannels)
      {
        return requiredSources;
      }

      for (std::size_t destination = 0U; destination < optOutput->size(); ++destination)
      {
        auto const source =
          observation.optOutputMap ? (*observation.optOutputMap)[destination] : static_cast<::SInt32>(destination);

        if (source >= 0 &&
            !tryConstrainSource(
              requiredSources[static_cast<std::size_t>(source)], (*optOutput)[destination], client.speakerMask))
        {
          return std::unexpected{roleFailure()};
        }
      }

      return requiredSources;
    }

    std::expected<void, BackendOpenFailure> completeSourcePermutation(
      std::span<std::uint32_t> const sourceSlots,
      std::span<std::optional<std::uint32_t> const> const requiredSources)
    {
      auto used = std::vector<bool>(sourceSlots.size(), false);

      for (auto const& optSource : requiredSources)
      {
        if (optSource)
        {
          if (used[*optSource])
          {
            return std::unexpected{roleFailure()};
          }

          used[*optSource] = true;
        }
      }

      for (std::size_t slot = 0U; slot < sourceSlots.size(); ++slot)
      {
        if (auto const& optSource = requiredSources[slot]; optSource)
        {
          sourceSlots[slot] = *optSource;
          continue;
        }

        // Unique constraints leave exactly enough unused sources for opaque slots.
        auto const unused = std::ranges::find(used, false);
        auto const source = static_cast<std::uint32_t>(unused - used.begin());
        sourceSlots[slot] = source;
        used[source] = true;
      }

      return {};
    }

    // The caller has validated current input storage and owns native access.
    // Read the remaining channel relationship without generation or publication
    // policy; the outer refresh owner discards retired successes and failures.
    std::expected<CoreAudioChannelRefresh, BackendOpenFailure> readCurrentChannelRelationship(
      CoreAudioChannelOperations const& operations,
      PcmFormat const& client,
      CoreAudioChannelPlan const& configured,
      CoreAudioChannelRefreshMode const mode)
    {
      auto refreshed = CoreAudioChannelRefresh{};
      auto optOutputChannels = std::optional<std::uint32_t>{};

      if (mode == CoreAudioChannelRefreshMode::Observation)
      {
        auto outputRes = readDownstreamFormat(operations);

        if (!outputRes)
        {
          return std::unexpected{outputRes.error()};
        }

        refreshed.optOutputFormat = std::move(*outputRes);

        if (refreshed.optOutputFormat)
        {
          optOutputChannels = refreshed.optOutputFormat->mChannelsPerFrame;
        }
      }

      // Neither the client count nor a device preference proves output geometry.
      // Current output labels/map can do so independently of downstream numerics.
      auto observationRes = observeChannels(operations, client.channels, optOutputChannels, false);

      if (!observationRes)
      {
        return std::unexpected{observationRes.error()};
      }

      if (observationRes->optOutputLabels)
      {
        optOutputChannels = static_cast<std::uint32_t>(observationRes->optOutputLabels->size());
      }
      else if (observationRes->optOutputMap)
      {
        optOutputChannels = static_cast<std::uint32_t>(observationRes->optOutputMap->size());
      }

      if (!optOutputChannels && mode == CoreAudioChannelRefreshMode::Execution)
      {
        auto outputRes = readDownstreamFormat(operations);

        if (!outputRes)
        {
          return std::unexpected{outputRes.error()};
        }

        refreshed.optOutputFormat = std::move(*outputRes);

        if (refreshed.optOutputFormat)
        {
          optOutputChannels = refreshed.optOutputFormat->mChannelsPerFrame;
        }
      }

      if (!optOutputChannels)
      {
        return std::unexpected{observationFailure("Core Audio current output geometry could not be proved")};
      }

      auto planRes = planCoreAudioChannels(client, *optOutputChannels, *observationRes);

      // Preference work is dispensable for execution when actual observations
      // already retain the provisioned permutation. Otherwise the fallback must
      // be freshly read, never copied from open-time evidence.
      if (!observationRes->optOutputLabels && planRes &&
          (mode == CoreAudioChannelRefreshMode::Observation || !canRetainCoreAudioChannelPlan(configured, *planRes)))
      {
        auto labelsRes =
          readLayoutProperty(operations, CoreAudioChannelProperty::DeviceLayout, client.channels, optOutputChannels);

        if (!labelsRes)
        {
          return std::unexpected{labelsRes.error()};
        }

        observationRes->optDeviceLabels = std::move(*labelsRes);
        planRes = planCoreAudioChannels(client, *optOutputChannels, *observationRes);
      }

      if (!planRes)
      {
        auto failure = planRes.error();
        failure.cause = BackendOpenFailure::Cause::OutputFailure;
        return std::unexpected{std::move(failure)};
      }

      if (!canRetainCoreAudioChannelPlan(configured, *planRes))
      {
        return std::unexpected{observationFailure("Core Audio current channel plan requires different render slots")};
      }

      refreshed.plan = std::move(*planRes);
      return refreshed;
    }
  } // namespace

  CoreAudioChannelOperations coreAudioChannelOperations(::AudioUnit const unit, ::AudioObjectID const device)
  {
    auto const deviceLayoutAddress =
      ::AudioObjectPropertyAddress{.mSelector = ::kAudioDevicePropertyPreferredChannelLayout,
                                   .mScope = ::kAudioObjectPropertyScopeOutput,
                                   .mElement = ::kAudioObjectPropertyElementMain};
    auto const unitPropertyFor = [](CoreAudioChannelProperty const property) -> ::AudioUnitPropertyID
    {
      if (property == CoreAudioChannelProperty::OutputMap)
      {
        return ::kAudioOutputUnitProperty_ChannelMap;
      }

      return ::kAudioUnitProperty_AudioChannelLayout;
    };
    auto const scopeFor = [](CoreAudioChannelProperty const property)
    { return property == CoreAudioChannelProperty::OutputLayout ? ::kAudioUnitScope_Output : ::kAudioUnitScope_Input; };
    auto const readFormat = [unit](::AudioUnitScope const scope, ::AudioStreamBasicDescription& format)
    {
      auto size = static_cast<::UInt32>(sizeof(format));
      auto const status = ::AudioUnitGetProperty(unit, ::kAudioUnitProperty_StreamFormat, scope, 0U, &format, &size);
      return status == ::noErr && size != sizeof(format) ? ::kAudioHardwareBadPropertySizeError : status;
    };
    return {
      .size =
        [=](CoreAudioChannelProperty const property, ::UInt32& size)
      {
        if (property == CoreAudioChannelProperty::DeviceLayout)
        {
          return ::AudioObjectGetPropertyDataSize(device, &deviceLayoutAddress, 0U, nullptr, &size);
        }

        ::Boolean writable = false;
        return ::AudioUnitGetPropertyInfo(unit, unitPropertyFor(property), scopeFor(property), 0U, &size, &writable);
      },
      .read =
        [=](CoreAudioChannelProperty const property, ::UInt32& size, void* const bytes)
      {
        if (property == CoreAudioChannelProperty::DeviceLayout)
        {
          return ::AudioObjectGetPropertyData(device, &deviceLayoutAddress, 0U, nullptr, &size, bytes);
        }

        return ::AudioUnitGetProperty(unit, unitPropertyFor(property), scopeFor(property), 0U, bytes, &size);
      },
      .set =
        [=](CoreAudioChannelProperty const property, std::span<std::byte const> const bytes)
      {
        return ::AudioUnitSetProperty(
          unit, unitPropertyFor(property), scopeFor(property), 0U, bytes.data(), static_cast<::UInt32>(bytes.size()));
      },
      .readInputFormat = [=](::AudioStreamBasicDescription& format)
      { return readFormat(::kAudioUnitScope_Input, format); },
      .readOutputFormat = [=](::AudioStreamBasicDescription& format)
      { return readFormat(::kAudioUnitScope_Output, format); }};
  }

  std::expected<CoreAudioChannelObservation, BackendOpenFailure> observeCoreAudioChannels(
    CoreAudioChannelOperations const& operations,
    std::uint32_t const clientChannels,
    std::uint32_t const outputChannels)
  {
    return observeChannels(operations, clientChannels, outputChannels, true);
  }

  std::expected<CoreAudioChannelPlan, BackendOpenFailure> planCoreAudioChannels(
    PcmFormat const& client,
    ::AudioStreamBasicDescription const& output,
    CoreAudioChannelObservation const& observation)
  {
    auto const outputSignalRes = coreAudioSignalFormat(output);

    if (!outputSignalRes)
    {
      return std::unexpected{observationFailure("Core Audio returned an invalid downstream format")};
    }

    return planCoreAudioChannels(client, outputSignalRes->channels, observation);
  }

  std::expected<CoreAudioChannelPlan, BackendOpenFailure> planCoreAudioChannels(
    PcmFormat const& client,
    std::uint32_t const outputChannels,
    CoreAudioChannelObservation const& observation)
  {
    if (client.channels == 0U || outputChannels == 0U ||
        (observation.optClientLabels && observation.optClientLabels->size() != client.channels) ||
        (observation.optOutputLabels && observation.optOutputLabels->size() != outputChannels) ||
        (observation.optOutputMap && observation.optOutputMap->size() != outputChannels))
    {
      return std::unexpected{observationFailure("Core Audio channel plan requires matching bus observations")};
    }

    if (observation.optOutputMap && !hasValidMapSources(*observation.optOutputMap, client.channels))
    {
      return std::unexpected{observationFailure("Core Audio channel plan has an invalid source ordinal")};
    }

    auto plan = CoreAudioChannelPlan{};
    plan.delivery.mapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified;
    // Rate, precision and sample-domain changes remain independent format facts.
    plan.delivery.hasSharedConversion = hasChannelConversion(client.channels, outputChannels, observation.optOutputMap);
    plan.sourceSlots.resize(client.channels);

    for (std::uint32_t slot = 0U; slot < client.channels; ++slot)
    {
      plan.sourceSlots[slot] = slot;
    }

    projectNativeChannels(plan, client, outputChannels, observation);

    if (client.speakerMask == 0U)
    {
      return plan;
    }

    if (!std::cmp_equal(std::popcount(client.speakerMask), client.channels) ||
        (client.speakerMask & ~kSupportedWaveRoles) != 0U)
    {
      return std::unexpected{roleFailure()};
    }

    auto const sourcesRes =
      requiredChannelSources(client, outputChannels, observation, plan.delivery.hasSharedConversion);

    if (!sourcesRes)
    {
      return std::unexpected{sourcesRes.error()};
    }

    if (auto const permutationRes = completeSourcePermutation(plan.sourceSlots, *sourcesRes); !permutationRes)
    {
      return std::unexpected{permutationRes.error()};
    }

    for (std::uint32_t slot = 0U; slot < client.channels; ++slot)
    {
      if (plan.sourceSlots[slot] != slot)
      {
        plan.hasIdentityOrder = false;
        break;
      }
    }

    return plan;
  }

  std::expected<CoreAudioChannelPlan, BackendOpenFailure> configureCoreAudioChannels(
    CoreAudioChannelOperations const& operations,
    PcmFormat const& client,
    ::AudioStreamBasicDescription const& output)
  {
    auto observationRes = observeCoreAudioChannels(operations, client.channels, output.mChannelsPerFrame);

    if (!observationRes)
    {
      return std::unexpected{observationRes.error()};
    }

    auto planRes = planCoreAudioChannels(client, output, *observationRes);

    if (planRes || planRes.error().cause != BackendOpenFailure::Cause::TrackRouteIncompatible || !operations.set ||
        !std::cmp_equal(std::popcount(client.speakerMask), client.channels) || !observationRes->optClientLabels ||
        output.mChannelsPerFrame != client.channels)
    {
      return planRes;
    }

    auto const& optOutput =
      observationRes->optOutputLabels ? observationRes->optOutputLabels : observationRes->optDeviceLabels;

    if (!optOutput || optOutput->size() != client.channels)
    {
      return planRes;
    }

    auto const& nativeClient = *observationRes->optClientLabels;
    auto map = std::vector<::SInt32>{};
    map.reserve(client.channels);

    // Only complete, independently observed same-set roles justify changing the
    // local output unit map. No device setting or count-derived role is written.
    for (auto const label : *optOutput)
    {
      auto const role = waveRole(label);
      auto const source = std::ranges::find(nativeClient, label);

      if (role == 0U || (client.speakerMask & role) == 0U || source == nativeClient.end())
      {
        return planRes;
      }

      map.push_back(static_cast<::SInt32>(source - nativeClient.begin()));
    }

    auto const status =
      operations.set(CoreAudioChannelProperty::OutputMap, std::as_bytes(std::span<::SInt32 const>{map}));

    if (status != ::noErr)
    {
      if (isUnavailable(status, CoreAudioChannelProperty::OutputMap) || status == ::kAudioUnitErr_FormatNotSupported)
      {
        return std::unexpected{BackendOpenFailure{
          .error = makeCoreAudioError(
                     status, "configure lossless Core Audio output channel relationship", Error::Code::FormatRejected)
                     .error(),
          .cause = BackendOpenFailure::Cause::TrackRouteIncompatible}};
      }

      return std::unexpected{BackendOpenFailure{
        .error = makeCoreAudioError(status, "configure Core Audio output channel map", Error::Code::InitFailed).error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    observationRes = observeCoreAudioChannels(operations, client.channels, output.mChannelsPerFrame);

    if (!observationRes)
    {
      return std::unexpected{observationRes.error()};
    }

    if (!observationRes->optOutputMap || *observationRes->optOutputMap != map)
    {
      return std::unexpected{observationFailure("Core Audio applied a different output channel map than requested")};
    }

    // After successful SET, contradictory observations are an output fault,
    // not a capability refusal. Keep the original owning error intact.
    return planCoreAudioChannels(client, output, *observationRes)
      .transform_error(
        [](BackendOpenFailure failure)
        {
          failure.cause = BackendOpenFailure::Cause::OutputFailure;
          return failure;
        });
  }

  std::expected<std::optional<CoreAudioChannelRefresh>, BackendOpenFailure> refreshCoreAudioChannels(
    CoreAudioChannelOperations const& operations,
    PcmFormat const& client,
    CoreAudioChannelPlan const& configured,
    CoreAudioChannelRefreshMode const mode,
    std::function<bool()> const& isCurrent)
  {
    if (!isCurrent())
    {
      return std::nullopt;
    }

    auto refreshRes = [&] -> std::expected<CoreAudioChannelRefresh, BackendOpenFailure>
    {
      if (!operations.readInputFormat || !operations.readOutputFormat)
      {
        return std::unexpected{observationFailure("Core Audio refresh requires current format readbacks")};
      }

      auto input = ::AudioStreamBasicDescription{};

      if (auto const status = operations.readInputFormat(input); status != ::noErr)
      {
        return std::unexpected{BackendOpenFailure{
          .error = makeCoreAudioError(
                     status,
                     "read Core Audio current client format",
                     isCoreAudioDeviceLossStatus(status) ? Error::Code::DeviceNotFound : Error::Code::InitFailed)
                     .error(),
          .cause = BackendOpenFailure::Cause::OutputFailure}};
      }

      if (auto const requestedRes = coreAudioFormat(client);
          !requestedRes || !isSameCoreAudioPcmFormat(*requestedRes, input))
      {
        return std::unexpected{observationFailure("Core Audio current client PCM mode changed")};
      }

      return readCurrentChannelRelationship(operations, client, configured, mode);
    }();

    // Retirement discards both success and failure, including an input getter
    // that failed after stop/close advanced the generation during the native call.
    if (!isCurrent())
    {
      return std::nullopt;
    }

    if (!refreshRes)
    {
      return std::unexpected{refreshRes.error()};
    }

    return std::optional{std::move(*refreshRes)};
  }

  OpenedPcmMode coreAudioOpenedPcmMode(PcmFormat const& renderFormat, CoreAudioChannelPlan const& plan) noexcept
  {
    return {.clientFormat = renderFormat, .channelDelivery = plan.delivery};
  }

  bool canRetainCoreAudioChannelPlan(CoreAudioChannelPlan const& configured,
                                     CoreAudioChannelPlan const& refreshed) noexcept
  {
    return configured.sourceSlots == refreshed.sourceSlots;
  }

  void copyCoreAudioChannels(std::span<std::byte> const output,
                             std::span<std::byte const> const input,
                             std::size_t const sampleBytes,
                             std::span<std::uint32_t const> const sourceSlots) noexcept
  {
    auto const frameBytes = sampleBytes * sourceSlots.size();
    AO_RT_INVARIANT(frameBytes != 0U && output.size() == input.size() && input.size() % frameBytes == 0U,
                    "Core Audio channel copy requires whole matching PCM frames");

    for (std::size_t frame = 0U; frame < input.size(); frame += frameBytes)
    {
      for (std::size_t destination = 0U; destination < sourceSlots.size(); ++destination)
      {
        std::memcpy(output.data() + frame + (destination * sampleBytes),
                    input.data() + frame + (sourceSlots[destination] * sampleBytes),
                    sampleBytes);
      }
    }
  }
} // namespace ao::audio::backend::detail

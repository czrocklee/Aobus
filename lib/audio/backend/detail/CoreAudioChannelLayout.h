// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>

#include <AudioToolbox/AudioUnit.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <MacTypes.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ao::audio::backend::detail
{
  enum class CoreAudioChannelProperty : std::uint8_t
  {
    ClientLayout,
    OutputLayout,
    DeviceLayout,
    OutputMap,
  };

  // The same bounded property operations drive native open and controlled fixtures.
  struct CoreAudioChannelOperations final
  {
    std::function<::OSStatus(CoreAudioChannelProperty, ::UInt32&)> size{};
    std::function<::OSStatus(CoreAudioChannelProperty, ::UInt32&, void*)> read{};
    std::function<::OSStatus(CoreAudioChannelProperty, std::span<std::byte const>)> set{};
    // Native implementations reject successful reads with a nonexact ASBD size.
    std::function<::OSStatus(::AudioStreamBasicDescription&)> readInputFormat{};
    std::function<::OSStatus(::AudioStreamBasicDescription&)> readOutputFormat{};
  };

  struct CoreAudioChannelObservation final
  {
    std::optional<std::vector<::AudioChannelLabel>> optClientLabels{};
    std::optional<std::vector<::AudioChannelLabel>> optOutputLabels{};
    std::optional<std::vector<::AudioChannelLabel>> optDeviceLabels{};
    std::optional<std::vector<::SInt32>> optOutputMap{};
    bool operator==(CoreAudioChannelObservation const&) const = default;
  };

  struct CoreAudioChannelPlan final
  {
    // Native client slot -> canonical render-target slot. Never endpoint evidence.
    std::vector<std::uint32_t> sourceSlots{};
    // Computed before callback admission and immutable until render quiescence.
    bool hasIdentityOrder = true;
    // Native storage, not the canonical application/render-target contract.
    PcmFormat nativeClientFormat{};
    ChannelDeliveryEvidence delivery{};
    ChannelDeliveryEvidence outputDelivery{};
  };

  enum class CoreAudioChannelRefreshMode : std::uint8_t
  {
    Execution,
    Observation,
  };

  struct CoreAudioChannelRefresh final
  {
    CoreAudioChannelPlan plan{};
    // Fresh downstream diagnostics only; absent is never a cached/synthetic ASBD.
    std::optional<::AudioStreamBasicDescription> optOutputFormat{};
  };

  CoreAudioChannelOperations coreAudioChannelOperations(::AudioUnit unit, ::AudioObjectID device);
  std::expected<CoreAudioChannelObservation, BackendOpenFailure> observeCoreAudioChannels(
    CoreAudioChannelOperations const& operations,
    std::uint32_t clientChannels,
    std::uint32_t outputChannels);
  std::expected<CoreAudioChannelPlan, BackendOpenFailure> planCoreAudioChannels(
    PcmFormat const& client,
    ::AudioStreamBasicDescription const& output,
    CoreAudioChannelObservation const& observation);
  std::expected<CoreAudioChannelPlan, BackendOpenFailure> planCoreAudioChannels(
    PcmFormat const& client,
    std::uint32_t outputChannels,
    CoreAudioChannelObservation const& observation);

  std::expected<CoreAudioChannelPlan, BackendOpenFailure> configureCoreAudioChannels(
    CoreAudioChannelOperations const& operations,
    PcmFormat const& client,
    ::AudioStreamBasicDescription const& output);

  // The caller holds nativeMutex; retirement may advance the generation while
  // a read is in flight. Obsolete/closed reads must never be published.
  // Both modes prove fresh input storage and the immutable permutation. Execution
  // omits downstream numerical diagnostics when current layouts/maps prove count,
  // and reads preferences only if needed to retain the configured permutation.
  std::expected<std::optional<CoreAudioChannelRefresh>, BackendOpenFailure> refreshCoreAudioChannels(
    CoreAudioChannelOperations const& operations,
    PcmFormat const& client,
    CoreAudioChannelPlan const& configured,
    CoreAudioChannelRefreshMode mode,
    std::function<bool()> const& isCurrent);

  // The native storage projection must never replace the render-target contract.
  OpenedPcmMode coreAudioOpenedPcmMode(PcmFormat const& renderFormat, CoreAudioChannelPlan const& plan) noexcept;

  bool canRetainCoreAudioChannelPlan(CoreAudioChannelPlan const& configured,
                                     CoreAudioChannelPlan const& refreshed) noexcept;
  void copyCoreAudioChannels(std::span<std::byte> output,
                             std::span<std::byte const> input,
                             std::size_t sampleBytes,
                             std::span<std::uint32_t const> sourceSlots) noexcept;
} // namespace ao::audio::backend::detail

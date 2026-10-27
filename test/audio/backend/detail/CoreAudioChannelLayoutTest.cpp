// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioChannelLayout.h"

#include "lib/audio/backend/detail/CoreAudioFormat.h"
#include "lib/audio/backend/detail/CoreAudioGraph.h"
#include "lib/audio/backend/detail/CoreAudioRenderBuffer.h"
#include <ao/Error.h>
#include <ao/audio/NodeFormat.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

#include <AudioToolbox/AUComponent.h>
#include <AudioToolbox/AudioFormat.h>
#include <AudioToolbox/AudioUnitProperties.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <MacTypes.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    using Property = CoreAudioChannelProperty;
    constexpr std::size_t kHeaderBytes = offsetof(::AudioChannelLayout, mChannelDescriptions);

    std::vector<std::byte> descriptions(std::initializer_list<::AudioChannelLabel> const labels)
    {
      auto bytes = std::vector<std::byte>(kHeaderBytes + (labels.size() * sizeof(::AudioChannelDescription)));
      auto const header = std::to_array<::UInt32>({0U, 0U, static_cast<::UInt32>(labels.size())});
      std::memcpy(bytes.data(), header.data(), kHeaderBytes);
      std::size_t offset = kHeaderBytes;

      for (auto const label : labels)
      {
        auto const description =
          ::AudioChannelDescription{.mChannelLabel = label, .mChannelFlags = 0U, .mCoordinates = {}};
        std::memcpy(bytes.data() + offset, &description, sizeof(description));
        offset += sizeof(description);
      }

      return bytes;
    }

    std::vector<std::byte> layoutHeader(::UInt32 const tag, ::UInt32 const bitmap = 0U)
    {
      auto const header = std::to_array<::UInt32>({tag, bitmap, 0U});
      auto const bytes = std::as_bytes(std::span<::UInt32 const>{header});
      return {bytes.begin(), bytes.end()};
    }

    std::vector<std::byte> ordinalMap(std::initializer_list<::SInt32> const map)
    {
      auto const bytes = std::as_bytes(std::span<::SInt32 const>{map.begin(), map.size()});
      return {bytes.begin(), bytes.end()};
    }

    std::vector<std::byte> markers(std::initializer_list<std::uint8_t> const bytes)
    {
      auto const view = std::as_bytes(std::span<std::uint8_t const>{bytes.begin(), bytes.size()});
      return {view.begin(), view.end()};
    }

    ::AudioStreamBasicDescription outputFormat(::UInt32 channels = 2U);

    struct PropertyReply final
    {
      std::optional<std::vector<std::byte>> optBytes{};
      ::OSStatus sizeStatus = ::noErr;
      ::OSStatus readStatus = ::noErr;
      bool shortRead = false;
    };

    struct ChannelObservationFake final
    {
      std::array<PropertyReply, 4> replies{};
      std::vector<Property> queries{};
      std::size_t reads = 0U;
      std::size_t sets = 0U;
      ::OSStatus setStatus = ::noErr;
      bool ignoreSet = false;
      // Controlled AudioFormat expansion replies; the defaults keep the
      // successful tag and bitmap observations on the real SDK calls.
      std::function<decltype(::AudioFormatGetPropertyInfo)> expandLayoutInfo{::AudioFormatGetPropertyInfo};
      std::function<decltype(::AudioFormatGetProperty)> expandLayout{::AudioFormatGetProperty};
      std::function<void()> afterRead{};
      ::AudioStreamBasicDescription inputFormat = outputFormat();
      ::AudioStreamBasicDescription downstreamFormat = outputFormat();
      ::OSStatus inputStatus = ::noErr;
      ::OSStatus outputStatus = ::noErr;
      ::UInt32 inputFormatBytes = static_cast<::UInt32>(sizeof(::AudioStreamBasicDescription));
      ::UInt32 outputFormatBytes = static_cast<::UInt32>(sizeof(::AudioStreamBasicDescription));
      ::UInt32 inputFormatCapacity = 0U;
      ::UInt32 outputFormatCapacity = 0U;
      std::size_t inputFormatReads = 0U;
      std::size_t outputFormatReads = 0U;
      std::function<void()> afterInputRead{};
      std::function<void()> afterOutputRead{};

      PropertyReply& reply(Property const property) { return replies[static_cast<std::size_t>(property)]; }

      CoreAudioChannelOperations operations()
      {
        return {.size = [this](Property const property, ::UInt32& size) -> ::OSStatus
                {
                  queries.push_back(property);
                  auto const& value = reply(property);

                  if (value.sizeStatus != ::noErr)
                  {
                    return value.sizeStatus;
                  }

                  if (!value.optBytes)
                  {
                    if (property == Property::DeviceLayout)
                    {
                      return ::kAudioHardwareUnknownPropertyError;
                    }

                    return ::kAudioUnitErr_PropertyNotInUse;
                  }

                  size = static_cast<::UInt32>(value.optBytes->size());
                  return ::noErr;
                },
                .read = [this](Property const property, ::UInt32& size, void* const output) -> ::OSStatus
                {
                  ++reads;
                  auto const& value = reply(property);
                  std::memcpy(output, value.optBytes->data(), value.optBytes->size());

                  if (value.shortRead)
                  {
                    --size;
                  }

                  if (afterRead)
                  {
                    afterRead();
                  }

                  return value.readStatus;
                },
                .set = [this](Property const property, std::span<std::byte const> const bytes) -> ::OSStatus
                {
                  ++sets;

                  if (setStatus == ::noErr && !ignoreSet)
                  {
                    reply(property).optBytes = std::vector<std::byte>{bytes.begin(), bytes.end()};
                  }

                  return setStatus;
                },
                .expandLayoutInfo = expandLayoutInfo,
                .expandLayout = expandLayout,
                .readInputFormat =
                  [this](::AudioStreamBasicDescription& format, ::UInt32& bytes)
                {
                  ++inputFormatReads;
                  inputFormatCapacity = bytes;
                  format = inputFormat;
                  bytes = inputFormatBytes;

                  if (afterInputRead)
                  {
                    afterInputRead();
                  }

                  return inputStatus;
                },
                .readOutputFormat =
                  [this](::AudioStreamBasicDescription& format, ::UInt32& bytes)
                {
                  ++outputFormatReads;
                  outputFormatCapacity = bytes;
                  format = downstreamFormat;
                  bytes = outputFormatBytes;

                  if (afterOutputRead)
                  {
                    afterOutputRead();
                  }

                  return outputStatus;
                }};
      }
    };

    ::AudioStreamBasicDescription outputFormat(::UInt32 const channels)
    {
      return {.mSampleRate = 48000.0,
              .mFormatID = ::kAudioFormatLinearPCM,
              .mFormatFlags = ::kAudioFormatFlagIsSignedInteger | ::kAudioFormatFlagIsPacked,
              .mBytesPerPacket = channels * 3U,
              .mFramesPerPacket = 1U,
              .mBytesPerFrame = channels * 3U,
              .mChannelsPerFrame = channels,
              .mBitsPerChannel = 24U,
              .mReserved = 0U};
    }

    constexpr auto kStereoClient =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed24PackedLe, .speakerMask = 0x3};

    // Both requested representations stay stereo, so only the expansion
    // dispatch can make the two failure variants diverge.
    constexpr ::UInt32 kStereoChannelLayoutTag = (101U << 16U) | 2U;
    constexpr ::UInt32 kStereoChannelBitmap = 0x3U;
    constexpr ::UInt32 kUseChannelBitmapTag = 1U << 16U;
    // A genuinely unrelated expanded tag: neither UseChannelDescriptions, a
    // retained requested tag, nor UseChannelBitmap itself, which a bitmap
    // request asks for and would therefore wrongly accept.
    constexpr ::UInt32 kUnrelatedExpandedLayoutTag = ::kAudioChannelLayoutTag_Quadraphonic;

    struct ExpansionSpecifier final
    {
      ::AudioFormatPropertyID property = 0U;
      ::UInt32 specifierSize = 0U;
      bool hasSpecifier = false;
      ::UInt32 specifier = 0U;
    };

    struct ExpansionCalls final
    {
      std::size_t infoCalls = 0U;
      std::size_t expandCalls = 0U;
      std::vector<ExpansionSpecifier> infoSpecifiers{};
      std::vector<ExpansionSpecifier> expandSpecifiers{};
    };

    bool matchesExpansionRequest(ExpansionSpecifier const& record,
                                 ::AudioFormatPropertyID const property,
                                 ::UInt32 const specifier) noexcept
    {
      return record.property == property && record.specifierSize == sizeof(::UInt32) && record.hasSpecifier &&
             record.specifier == specifier;
    }

    // Deterministic AudioFormat expansion faults; the real-SDK tag and bitmap
    // observations elsewhere keep their native defaults.
    enum class ExpansionFaultMode : std::uint8_t
    {
      InfoStatus,
      GetStatus,
      TooSmallSize,
      OverboundSize,
      ChangedExtent,
      WrongExpandedTag,
      ZeroExpandedCount,
    };

    bool hasExpansionGetCall(ExpansionFaultMode const mode) noexcept
    {
      return mode == ExpansionFaultMode::GetStatus || mode == ExpansionFaultMode::ChangedExtent ||
             mode == ExpansionFaultMode::WrongExpandedTag || mode == ExpansionFaultMode::ZeroExpandedCount;
    }

    bool isExpansionStatusFault(ExpansionFaultMode const mode) noexcept
    {
      return mode == ExpansionFaultMode::InfoStatus || mode == ExpansionFaultMode::GetStatus;
    }

    // Single source of the injected native statuses; both are non-loss faults
    // that classify through the InitFailed fallback.
    ::OSStatus expansionInjectedStatus(ExpansionFaultMode const mode) noexcept
    {
      switch (mode)
      {
        case ExpansionFaultMode::InfoStatus: return ::kAudioHardwareBadPropertySizeError;
        case ExpansionFaultMode::GetStatus: return ::kAudioHardwareIllegalOperationError;
        default: return ::noErr;
      }
    }

    std::string_view expansionFailureMessage(ExpansionFaultMode const mode) noexcept
    {
      switch (mode)
      {
        case ExpansionFaultMode::InfoStatus: return "size expanded Core Audio channel layout";
        case ExpansionFaultMode::GetStatus: return "expand Core Audio channel layout";
        case ExpansionFaultMode::TooSmallSize:
        case ExpansionFaultMode::OverboundSize: return "Core Audio returned an unexpandable channel layout";
        case ExpansionFaultMode::ChangedExtent: return "Core Audio channel layout expansion changed size";
        case ExpansionFaultMode::WrongExpandedTag:
        case ExpansionFaultMode::ZeroExpandedCount: return "Core Audio returned malformed channel descriptions";
      }

      return {};
    }

    // Copies the bounded native specifier value off the production stack; the
    // pointer itself is never retained, and a null pointer or an unexpected
    // size records no value instead of reading one.
    ExpansionSpecifier makeExpansionSpecifier(::AudioFormatPropertyID const property,
                                              ::UInt32 const specifierSize,
                                              void const* const specifier) noexcept
    {
      auto record = ExpansionSpecifier{};
      record.property = property;
      record.specifierSize = specifierSize;
      record.hasSpecifier = specifier != nullptr && record.specifierSize == sizeof(::UInt32);

      if (record.hasSpecifier)
      {
        std::memcpy(&record.specifier, specifier, sizeof(record.specifier));
      }

      return record;
    }

    struct ExpansionRepresentation final
    {
      std::vector<std::byte> layout{};
      ::AudioFormatPropertyID property = 0U;
      ::UInt32 specifier = 0U;
    };

    // The standard stereo tag versus the stereo bitmap: both representations
    // request the same roles, so only the requested property and specifier
    // differ.
    std::array<ExpansionRepresentation, 2> expansionRepresentations()
    {
      return {
        ExpansionRepresentation{.layout = layoutHeader(kStereoChannelLayoutTag),
                                .property = ::kAudioFormatProperty_ChannelLayoutForTag,
                                .specifier = kStereoChannelLayoutTag},
        ExpansionRepresentation{.layout = layoutHeader(kUseChannelBitmapTag, kStereoChannelBitmap),
                                .property = ::kAudioFormatProperty_ChannelLayoutForBitmap,
                                .specifier = kStereoChannelBitmap},
      };
    }

    void faultExpansion(ChannelObservationFake& fake, ExpansionFaultMode const mode, ExpansionCalls& calls)
    {
      fake.expandLayoutInfo =
        [&calls, mode](
          auto const property, auto const specifierSize, void const* const specifier, auto* const outSize) -> ::OSStatus
      {
        ++calls.infoCalls;
        calls.infoSpecifiers.push_back(makeExpansionSpecifier(property, specifierSize, specifier));

        switch (mode)
        {
          case ExpansionFaultMode::InfoStatus: return expansionInjectedStatus(mode);
          case ExpansionFaultMode::TooSmallSize: *outSize = static_cast<::UInt32>(kHeaderBytes - 1U); return ::noErr;
          case ExpansionFaultMode::OverboundSize: *outSize = 65537U; return ::noErr;
          default:
            *outSize = static_cast<::UInt32>(kHeaderBytes + (2U * sizeof(::AudioChannelDescription)));
            return ::noErr;
        }
      };
      // The native invocation passes the in/out byte size fourth and the SDK
      // out-data pointer fifth: (property, specifierSize, specifier, &expandedSize,
      // expanded.data()). The generic parameters mirror that exact native order.
      fake.expandLayout = [&calls, mode](auto const property,
                                         auto const specifierSize,
                                         void const* const specifier,
                                         auto* const ioSize,
                                         auto const outData) -> ::OSStatus
      {
        ++calls.expandCalls;
        calls.expandSpecifiers.push_back(makeExpansionSpecifier(property, specifierSize, specifier));
        auto const allocated = *ioSize;
        auto reply = std::vector<std::byte>{};

        switch (mode)
        {
          case ExpansionFaultMode::GetStatus: return expansionInjectedStatus(mode);
          case ExpansionFaultMode::ChangedExtent:
            reply = descriptions({1U, 2U});
            *ioSize = allocated - 1U;
            break;
          case ExpansionFaultMode::WrongExpandedTag:
          {
            reply = descriptions({1U, 2U});
            auto const header = std::to_array<::UInt32>({kUnrelatedExpandedLayoutTag, 0U, 2U});
            std::memcpy(reply.data(), header.data(), kHeaderBytes);
            break;
          }
          case ExpansionFaultMode::ZeroExpandedCount: reply = descriptions({}); break;
          default: reply = descriptions({1U, 2U}); break;
        }

        auto const copied =
          reply.size() < static_cast<std::size_t>(allocated) ? reply.size() : static_cast<std::size_t>(allocated);
        std::memcpy(outData, reply.data(), copied);
        return ::noErr;
      };
    }
  } // namespace

  TEST_CASE("CoreAudioChannelLayout - coordinate positions are unsupported identity rather than unpositioned slots")
  {
    auto fake = ChannelObservationFake{};
    auto coordinates = descriptions({::kAudioChannelLabel_UseCoordinates, ::kAudioChannelLabel_UseCoordinates});

    for (std::size_t slot = 0; slot < 2; ++slot)
    {
      auto description = ::AudioChannelDescription{};
      description.mChannelLabel = ::kAudioChannelLabel_UseCoordinates;
      description.mChannelFlags = ::kAudioChannelFlags_RectangularCoordinates;
      description.mCoordinates[0] = slot == 0 ? -1.0F : 1.0F;
      description.mCoordinates[1] = 1.0F;
      std::memcpy(coordinates.data() + kHeaderBytes + (slot * sizeof(description)), &description, sizeof(description));
    }

    SECTION("identified client coordinates cannot be silently relabelled")
    {
      fake.reply(Property::ClientLayout).optBytes = coordinates;
    }

    SECTION("applicable one-to-one output coordinates are not ignored")
    {
      fake.reply(Property::OutputLayout).optBytes = coordinates;
    }

    auto const observedRes = observeCoreAudioChannels(fake.operations(), 2, 2);
    REQUIRE(observedRes); // Equal label codes can identify distinct coordinates.
    auto const knownRes = planCoreAudioChannels(kStereoClient, outputFormat(), *observedRes);
    REQUIRE_FALSE(knownRes);
    CHECK(knownRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    auto unidentified = kStereoClient;
    unidentified.speakerMask = 0;
    auto const legacyRes = planCoreAudioChannels(unidentified, outputFormat(), *observedRes);
    REQUIRE(legacyRes);
    CHECK_FALSE(legacyRes->delivery.isMapped);
    CHECK(legacyRes->delivery.nativeSpeakerMask == 0);
    CHECK_FALSE(legacyRes->delivery.hasSharedConversion);
  }

  TEST_CASE("CoreAudioChannelLayout - unavailable properties keep canonical compatibility and known conversion")
  {
    auto fake = ChannelObservationFake{};
    auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 6U);
    REQUIRE(observedRes);
    CHECK_FALSE(observedRes->optClientLabels);
    CHECK_FALSE(observedRes->optOutputLabels);
    CHECK_FALSE(observedRes->optDeviceLabels);
    CHECK_FALSE(observedRes->optOutputMap);
    CHECK(fake.queries ==
          std::vector<Property>{
            Property::ClientLayout, Property::OutputLayout, Property::DeviceLayout, Property::OutputMap});
    CHECK(fake.reads == 0U);
    auto output = outputFormat(6U);
    output.mSampleRate = 44100.0;
    output.mFormatFlags = ::kAudioFormatFlagIsFloat | ::kAudioFormatFlagIsPacked;
    output.mBitsPerChannel = 32U;
    output.mBytesPerPacket = 24U;
    output.mBytesPerFrame = 24U;
    auto const planRes = planCoreAudioChannels(kStereoClient, output, *observedRes);
    REQUIRE(planRes);
    CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{0U, 1U});
    CHECK_FALSE(planRes->delivery.isMapped);
    CHECK(planRes->delivery.nativeSpeakerMask == 0U);
    CHECK(planRes->delivery.hasSharedConversion);
  }

  TEST_CASE("CoreAudioChannelLayout - AU client layout admits only documented absence statuses")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::ClientLayout).optBytes = descriptions({1U, 2U});

    SECTION("unimplemented unit property is unavailable")
    {
      fake.reply(Property::ClientLayout).sizeStatus = ::kAudioUnitErr_InvalidProperty;
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
      REQUIRE(observedRes);
      CHECK_FALSE(observedRes->optClientLabels);
      CHECK(fake.reads == 0U);
    }

    SECTION("property becoming unused during read is unavailable")
    {
      fake.reply(Property::ClientLayout).readStatus = ::kAudioUnitErr_PropertyNotInUse;
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
      REQUIRE(observedRes);
      CHECK_FALSE(observedRes->optClientLabels);
      CHECK(fake.reads == 1U);
    }

    SECTION("a real native sizing fault is terminal")
    {
      fake.reply(Property::ClientLayout).sizeStatus = ::kAudioHardwareBadDeviceError;
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
      REQUIRE_FALSE(observedRes);
      CHECK(observedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK(observedRes.error().error.message.contains("size Core Audio client channel layout"));
    }

    SECTION("a real native read fault is terminal")
    {
      fake.reply(Property::ClientLayout).readStatus = ::kAudioUnitErr_Uninitialized;
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
      REQUIRE_FALSE(observedRes);
      CHECK(observedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK(fake.reads == 1U);
    }
  }

  TEST_CASE("CoreAudioChannelLayout - channel property device loss keeps its classification at size and read")
  {
    for (auto const property :
         {Property::ClientLayout, Property::OutputLayout, Property::DeviceLayout, Property::OutputMap})
    {
      for (auto const status :
           {::kAudioHardwareBadDeviceError, ::kAudioHardwareBadObjectError, ::kAudioHardwareNotRunningError})
      {
        for (auto const failsDuringRead : {false, true})
        {
          CAPTURE(property, status, failsDuringRead);
          auto fake = ChannelObservationFake{};
          auto& reply = fake.reply(property);
          reply.optBytes = property == Property::OutputMap ? ordinalMap({0, 1}) : descriptions({1U, 2U});

          if (failsDuringRead)
          {
            reply.readStatus = status;
            reply.shortRead = true; // Native failure takes priority over successful-read shape checks.
          }
          else
          {
            reply.sizeStatus = status;
          }

          auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
          REQUIRE_FALSE(observedRes);
          CHECK(observedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
          CHECK(observedRes.error().error.code == Error::Code::DeviceNotFound);
          CHECK(observedRes.error().error.message.contains(failsDuringRead ? "read Core Audio" : "size Core Audio"));
          CHECK(observedRes.error().error.message.contains("OSStatus"));
          CHECK(fake.reads == (failsDuringRead ? 1U : 0U));
          CHECK(fake.sets == 0U);
        }
      }
    }
  }

  TEST_CASE("CoreAudioChannelLayout - malformed successful observations are output failures")
  {
    auto fake = ChannelObservationFake{};

    SECTION("truncated layout header")
    {
      fake.reply(Property::ClientLayout).optBytes = markers({0, 0, 0, 0});
    }

    SECTION("a layout for another client bus count")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({1U, 2U, 3U});
    }

    SECTION("duplicate identified descriptions")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({1U, 1U});
    }

    SECTION("truncated current output layout is not an optional preference")
    {
      fake.reply(Property::OutputLayout).optBytes = markers({0, 0, 0, 0});
    }

    SECTION("duplicate current output roles are not an optional preference")
    {
      fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 1U});
    }

    SECTION("successful size without a complete read")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({1U, 2U});
      fake.reply(Property::ClientLayout).shortRead = true;
    }

    SECTION("map has too few destinations")
    {
      fake.reply(Property::OutputMap).optBytes = ordinalMap({0});
    }

    SECTION("map references a missing source")
    {
      fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 2});
    }

    SECTION("map has an invalid negative source")
    {
      fake.reply(Property::OutputMap).optBytes = ordinalMap({-2, 1});
    }

    auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
    REQUIRE_FALSE(observedRes);
    CHECK(observedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    auto unidentified = kStereoClient;
    unidentified.speakerMask = 0U;
    auto const planRes = configureCoreAudioChannels(fake.operations(), unidentified, outputFormat());
    REQUIRE_FALSE(planRes);
    CHECK(planRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
  }

  TEST_CASE("CoreAudioChannelLayout - native downstream geometry need not fit diagnostic signal fields")
  {
    auto output = outputFormat();

    SECTION("a positive fractional native rate is not an integer signal rate")
    {
      output.mSampleRate = 48000.5;
    }

    SECTION("a native destination count need not fit an eight bit signal count")
    {
      output = outputFormat(256U);
    }

    REQUIRE_FALSE(coreAudioSignalFormat(output));

    for (auto const speakerMask : {0U, 0x3U})
    {
      CAPTURE(speakerMask, output.mSampleRate, output.mChannelsPerFrame);
      auto fake = ChannelObservationFake{};
      auto client = kStereoClient;
      client.speakerMask = speakerMask;
      auto const planRes = configureCoreAudioChannels(fake.operations(), client, output);
      REQUIRE(planRes);
      CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{0U, 1U});
      CHECK(planRes->hasIdentityOrder);
      CHECK(planRes->nativeClientFormat.sampleRate == 48000U);
      CHECK(planRes->nativeClientFormat.channels == 2U);
      CHECK(planRes->nativeClientFormat.encoding == SampleEncoding::Signed24PackedLe);
      CHECK(planRes->nativeClientFormat.speakerMask == 0U);
      CHECK_FALSE(planRes->delivery.isMapped);
      CHECK(planRes->delivery.nativeSpeakerMask == 0U);
      CHECK(planRes->outputDelivery.nativeSpeakerMask == 0U);
      CHECK(planRes->outputDelivery.hasSharedConversion == (output.mChannelsPerFrame == 256U));
      auto const opened = coreAudioOpenedPcmMode(client, *planRes);
      CHECK(opened.clientFormat == client);
      CHECK_FALSE(opened.optEndpoint);
      CHECK(fake.sets == 0U);
    }
  }

  TEST_CASE("CoreAudioChannelLayout - unusable native output geometry still rejects initial admission")
  {
    auto output = outputFormat();

    SECTION("zero native rate")
    {
      output.mSampleRate = 0.0;
    }

    SECTION("negative native rate")
    {
      output.mSampleRate = -48000.0;
    }

    SECTION("nonfinite native rate")
    {
      output.mSampleRate = std::numeric_limits<double>::infinity();
    }

    SECTION("not a number native rate")
    {
      output.mSampleRate = std::numeric_limits<double>::quiet_NaN();
    }

    SECTION("zero native destinations")
    {
      output = outputFormat(0U);
    }

    for (auto const speakerMask : {0U, 0x3U})
    {
      CAPTURE(speakerMask);
      auto client = kStereoClient;
      client.speakerMask = speakerMask;
      auto const planRes = planCoreAudioChannels(client, output, {});
      REQUIRE_FALSE(planRes);
      CHECK(planRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK(planRes.error().error.code == Error::Code::InitFailed);
      CHECK(planRes.error().error.message == "Core Audio returned invalid downstream geometry");
    }
  }

  TEST_CASE("CoreAudioChannelLayout - numerical format changes do not invent channel conversion")
  {
    auto fake = ChannelObservationFake{};
    auto output = outputFormat();

    SECTION("24-bit PCM padded into a 32-bit container does not change channels")
    {
      output.mFormatFlags = ::kAudioFormatFlagIsSignedInteger;
      output.mBytesPerPacket = 8U;
      output.mBytesPerFrame = 8U;
    }

    SECTION("24-bit integer to 32-bit integer precision remains a separate format fact")
    {
      output.mBitsPerChannel = 32U;
      output.mBytesPerPacket = 8U;
      output.mBytesPerFrame = 8U;
    }

    SECTION("resampling alone does not intervene in channel mapping")
    {
      output.mSampleRate = 44100.0;
    }

    SECTION("integer to float conversion alone does not intervene in channel mapping")
    {
      output.mFormatFlags = ::kAudioFormatFlagIsFloat | ::kAudioFormatFlagIsPacked;
      output.mBitsPerChannel = 32U;
      output.mBytesPerPacket = 8U;
      output.mBytesPerFrame = 8U;
    }

    auto const planRes = configureCoreAudioChannels(fake.operations(), kStereoClient, output);
    REQUIRE(planRes);
    CHECK_FALSE(planRes->delivery.hasSharedConversion);
    CHECK_FALSE(planRes->delivery.isMapped);
    CHECK(planRes->delivery.nativeSpeakerMask == 0U);
  }

  TEST_CASE("CoreAudioChannelLayout - centered mono retains opaque native Mono and known stereo conversion")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    fake.reply(Property::DeviceLayout).optBytes = descriptions({1U, 2U});
    fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 0});
    auto client = kStereoClient;
    client.channels = 1;
    client.speakerMask = 0x4;
    ::AudioChannelLabel expectedNativeLabel = 3U;

    SECTION("explicit Center client is not contradicted by converted stereo destinations")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({3U});
    }

    SECTION("native Mono is retained without being relabelled Center")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({42U});
      expectedNativeLabel = 42U;
    }

    auto const observedRes = observeCoreAudioChannels(fake.operations(), 1U, 2U);
    REQUIRE(observedRes);
    REQUIRE(observedRes->optClientLabels);
    CHECK(observedRes->optClientLabels->size() == 1U);
    CHECK(observedRes->optClientLabels->front() == expectedNativeLabel);
    auto const planRes = configureCoreAudioChannels(fake.operations(), client, outputFormat());
    REQUIRE(planRes);
    CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{0U});
    CHECK(planRes->delivery.hasSharedConversion);
    CHECK_FALSE(planRes->delivery.isMapped);
    CHECK(planRes->delivery.nativeSpeakerMask == (expectedNativeLabel == 3U ? 0x4U : 0U));
    CHECK(planRes->outputDelivery.nativeSpeakerMask == 0x3U);
    CHECK(planRes->nativeClientFormat.speakerMask == (expectedNativeLabel == 3U ? 0x4U : 0U));
    CHECK(fake.sets == 0U);
  }

  TEST_CASE("CoreAudioChannelLayout - native tags and bitmaps expand through AudioFormat rather than WAVE casts")
  {
    auto fake = ChannelObservationFake{};

    SECTION("MPEG three-channel tag has center first")
    {
      fake.reply(Property::ClientLayout).optBytes = layoutHeader((114U << 16U) | 3U);
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 3U, 3U);
      INFO((observedRes ? "MPEG tag observation succeeded" : observedRes.error().error.message));
      REQUIRE(observedRes);
      REQUIRE(observedRes->optClientLabels);
      CHECK(*observedRes->optClientLabels == std::vector<::AudioChannelLabel>{3U, 1U, 2U});
      auto client = kStereoClient;
      client.channels = 3;
      client.speakerMask = 0x7;
      auto const planRes = planCoreAudioChannels(client, outputFormat(3U), *observedRes);
      REQUIRE(planRes);
      CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{2U, 0U, 1U});
      CHECK(planRes->nativeClientFormat.speakerMask == 0U);
      CHECK(planRes->delivery.nativeSpeakerMask == 0x7U);
    }

    SECTION("bitmap expands left and center, not stereo by count")
    {
      fake.reply(Property::DeviceLayout).optBytes = layoutHeader(1U << 16U, 0x5U);
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
      INFO((observedRes ? "Bitmap observation succeeded" : observedRes.error().error.message));
      REQUIRE(observedRes);
      REQUIRE(observedRes->optDeviceLabels);
      CHECK(*observedRes->optDeviceLabels == std::vector<::AudioChannelLabel>{1U, 3U});
    }

    SECTION("stereo tag expands in native order")
    {
      fake.reply(Property::ClientLayout).optBytes = layoutHeader((101U << 16U) | 2U);
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
      INFO((observedRes ? "Stereo tag observation succeeded" : observedRes.error().error.message));
      REQUIRE(observedRes);
      REQUIRE(observedRes->optClientLabels);
      CHECK(*observedRes->optClientLabels == std::vector<::AudioChannelLabel>{1U, 2U});
    }
  }

  TEST_CASE("CoreAudioChannelLayout - required actual layout expansion failures remain output failures")
  {
    for (auto const& representation : expansionRepresentations())
    {
      for (auto const property : {Property::ClientLayout, Property::OutputLayout})
      {
        for (auto const mode : {ExpansionFaultMode::InfoStatus,
                                ExpansionFaultMode::GetStatus,
                                ExpansionFaultMode::TooSmallSize,
                                ExpansionFaultMode::OverboundSize,
                                ExpansionFaultMode::ChangedExtent,
                                ExpansionFaultMode::WrongExpandedTag,
                                ExpansionFaultMode::ZeroExpandedCount})
        {
          CAPTURE(property, mode, representation.specifier);
          auto fake = ChannelObservationFake{};
          fake.reply(property).optBytes = representation.layout;
          auto calls = ExpansionCalls{};
          faultExpansion(fake, mode, calls);
          std::size_t maskIndex = 0;

          for (auto const speakerMask : {0U, 0x3U})
          {
            CAPTURE(speakerMask);
            auto client = kStereoClient;
            client.speakerMask = speakerMask;
            auto const planRes = configureCoreAudioChannels(fake.operations(), client, outputFormat());
            REQUIRE_FALSE(planRes);
            CHECK(planRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
            CHECK(planRes.error().error.code == Error::Code::InitFailed);
            CHECK(planRes.error().error.message.contains(expansionFailureMessage(mode)));

            if (isExpansionStatusFault(mode))
            {
              CHECK(planRes.error().error.message.contains("failed (OSStatus"));
              CHECK(planRes.error().error.message.contains(std::format("{}", expansionInjectedStatus(mode))));
              CHECK(planRes.error().error.location.line() != 0U);
            }

            CHECK(fake.reads == maskIndex + 1U);
            CHECK(calls.infoCalls == maskIndex + 1U);
            CHECK(calls.expandCalls == (hasExpansionGetCall(mode) ? maskIndex + 1U : 0U));
            CHECK(calls.infoSpecifiers.size() == calls.infoCalls);
            CHECK(calls.expandSpecifiers.size() == calls.expandCalls);

            for (auto const& record : calls.infoSpecifiers)
            {
              CHECK(matchesExpansionRequest(record, representation.property, representation.specifier));
            }

            for (auto const& record : calls.expandSpecifiers)
            {
              CHECK(matchesExpansionRequest(record, representation.property, representation.specifier));
            }

            CHECK(fake.sets == 0U);
            ++maskIndex;
          }
        }
      }
    }
  }

  TEST_CASE("CoreAudioChannelLayout - unusable preferred layout expansions withhold evidence without vetoing transport")
  {
    for (auto const& representation : expansionRepresentations())
    {
      for (auto const mode : {ExpansionFaultMode::InfoStatus,
                              ExpansionFaultMode::GetStatus,
                              ExpansionFaultMode::TooSmallSize,
                              ExpansionFaultMode::OverboundSize,
                              ExpansionFaultMode::ChangedExtent,
                              ExpansionFaultMode::WrongExpandedTag,
                              ExpansionFaultMode::ZeroExpandedCount})
      {
        CAPTURE(mode, representation.specifier);
        auto fake = ChannelObservationFake{};
        fake.reply(Property::ClientLayout).optBytes = descriptions({1U, 2U});
        fake.reply(Property::DeviceLayout).optBytes = representation.layout;
        auto calls = ExpansionCalls{};
        faultExpansion(fake, mode, calls);
        auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
        REQUIRE(observedRes);
        REQUIRE(observedRes->optClientLabels);
        CHECK(*observedRes->optClientLabels == std::vector<::AudioChannelLabel>{1U, 2U});
        CHECK_FALSE(observedRes->optOutputLabels);
        CHECK_FALSE(observedRes->optDeviceLabels);
        CHECK_FALSE(observedRes->optOutputMap);
        CHECK(fake.queries ==
              std::vector<Property>{
                Property::ClientLayout, Property::OutputLayout, Property::DeviceLayout, Property::OutputMap});
        CHECK(fake.reads == 2U);
        CHECK(calls.infoCalls == 1U);
        CHECK(calls.expandCalls == (hasExpansionGetCall(mode) ? 1U : 0U));
        CHECK(calls.infoSpecifiers.size() == calls.infoCalls);
        CHECK(calls.expandSpecifiers.size() == calls.expandCalls);

        for (auto const& record : calls.infoSpecifiers)
        {
          CHECK(matchesExpansionRequest(record, representation.property, representation.specifier));
        }

        for (auto const& record : calls.expandSpecifiers)
        {
          CHECK(matchesExpansionRequest(record, representation.property, representation.specifier));
        }

        for (auto const speakerMask : {0U, 0x3U})
        {
          CAPTURE(speakerMask);
          auto client = kStereoClient;
          client.speakerMask = speakerMask;
          auto const planRes = planCoreAudioChannels(client, outputFormat(), *observedRes);
          REQUIRE(planRes);
          CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{0U, 1U});
          CHECK(planRes->hasIdentityOrder);
          CHECK_FALSE(planRes->delivery.isMapped);
          // Independently observed client roles are actual evidence, not fabrication.
          CHECK(planRes->delivery.nativeSpeakerMask == 0x3U);
          CHECK(planRes->nativeClientFormat.speakerMask == 0x3U);
          // The unusable preference supplies no downstream roles.
          CHECK(planRes->outputDelivery.nativeSpeakerMask == 0U);
          CHECK_FALSE(planRes->delivery.hasSharedConversion);
        }

        CHECK(fake.sets == 0U);
      }
    }
  }

  TEST_CASE("CoreAudioChannelLayout - known client conflicts cannot disappear into legacy uncertainty")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::ClientLayout).optBytes = descriptions({3U, 2U});
    auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
    REQUIRE(observedRes);
    auto const planRes = planCoreAudioChannels(kStereoClient, outputFormat(), *observedRes);
    REQUIRE_FALSE(planRes);
    CHECK(planRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
  }

  TEST_CASE("CoreAudioChannelLayout - Center and Mono are not unconditional aliases")
  {
    auto fake = ChannelObservationFake{};
    auto client = kStereoClient;
    client.channels = 1;
    client.speakerMask = 0x4;

    SECTION("explicit Center is compatible without claiming physical mono gain")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({3U});
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 1U, 1U);
      REQUIRE(observedRes);
      auto const planRes = planCoreAudioChannels(client, outputFormat(1U), *observedRes);
      REQUIRE(planRes);
      CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{0U});
      CHECK(planRes->nativeClientFormat.speakerMask == 0x4U);
      CHECK(planRes->delivery.nativeSpeakerMask == 0x4U);
      CHECK_FALSE(planRes->delivery.isMapped);
    }

    SECTION("explicit Mono remains opaque rather than proving or contradicting Center")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({42U});
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 1U, 1U);
      REQUIRE(observedRes);
      REQUIRE(observedRes->optClientLabels);
      CHECK(*observedRes->optClientLabels == std::vector<::AudioChannelLabel>{42U});
      auto const planRes = planCoreAudioChannels(client, outputFormat(1U), *observedRes);
      REQUIRE(planRes);
      CHECK_FALSE(planRes->delivery.isMapped);
      CHECK(planRes->delivery.nativeSpeakerMask == 0U);
      CHECK_FALSE(planRes->delivery.hasSharedConversion);
    }

    SECTION("unknown label does not become Center from the source or count")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({0xFFFFFFFFU});
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 1U, 1U);
      REQUIRE(observedRes);
      REQUIRE(observedRes->optClientLabels);
      CHECK(*observedRes->optClientLabels == std::vector<::AudioChannelLabel>{0xFFFFFFFFU});
      auto const planRes = planCoreAudioChannels(client, outputFormat(1U), *observedRes);
      REQUIRE(planRes);
      CHECK(planRes->delivery.nativeSpeakerMask == 0U);
      CHECK_FALSE(planRes->delivery.isMapped);
    }
  }

  TEST_CASE("CoreAudioChannelLayout - device preferences require applicable count and observed ordinal correspondence")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::DeviceLayout).optBytes = descriptions({3U, 4U, 1U, 2U, 5U, 6U});

    SECTION("larger preferred device layout alone is not the smaller client layout")
    {
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
      REQUIRE(observedRes);
      auto const planRes = planCoreAudioChannels(kStereoClient, outputFormat(), *observedRes);
      REQUIRE(planRes);
      CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{0U, 1U});
    }

    SECTION("observed map connects the client to the left and right device ordinals")
    {
      fake.reply(Property::OutputMap).optBytes = ordinalMap({-1, -1, 0, 1, -1, -1});
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 6U);
      REQUIRE(observedRes);
      auto const planRes = planCoreAudioChannels(kStereoClient, outputFormat(6U), *observedRes);
      REQUIRE(planRes);
      CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{0U, 1U});
      CHECK(planRes->delivery.hasSharedConversion);
    }

    SECTION("same-count preferred order constrains the client only as a fallback")
    {
      fake.reply(Property::DeviceLayout).optBytes = descriptions({2U, 1U});
      auto const planRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
      REQUIRE(planRes);
      CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{1U, 0U});
      CHECK_FALSE(planRes->hasIdentityOrder);
      CHECK(planRes->delivery.nativeSpeakerMask == 0U);
      CHECK(planRes->outputDelivery.nativeSpeakerMask == 0x3U);
      CHECK(fake.sets == 0U);
    }

    SECTION("a one-to-one same-count map to Center and LFE is not an unknown route")
    {
      fake.reply(Property::DeviceLayout).optBytes = descriptions({3U, 4U});
      fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
      auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
      REQUIRE(observedRes);
      auto const planRes = planCoreAudioChannels(kStereoClient, outputFormat(), *observedRes);
      REQUIRE_FALSE(planRes);
      CHECK(planRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    }
  }

  TEST_CASE("CoreAudioChannelLayout - current output layout overrides conflicting same-count device preferences")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});

    SECTION("preference reverses the current output order")
    {
      fake.reply(Property::DeviceLayout).optBytes = descriptions({2U, 1U});
    }

    SECTION("preference identifies different output roles")
    {
      fake.reply(Property::DeviceLayout).optBytes = descriptions({3U, 4U});
    }

    SECTION("preference has a truncated header")
    {
      fake.reply(Property::DeviceLayout).optBytes = markers({0, 0, 0, 0});
    }

    SECTION("preference has duplicate identified roles")
    {
      fake.reply(Property::DeviceLayout).optBytes = descriptions({1U, 1U});
    }

    SECTION("preference sizing would fail")
    {
      fake.reply(Property::DeviceLayout).sizeStatus = ::kAudioHardwareBadDeviceError;
    }

    SECTION("preference reading would fail")
    {
      fake.reply(Property::DeviceLayout).optBytes = descriptions({1U, 2U});
      fake.reply(Property::DeviceLayout).readStatus = ::kAudioUnitErr_Uninitialized;
    }

    for (auto const speakerMask : {0U, 0x3U})
    {
      for (auto const hasMap : {false, true})
      {
        CAPTURE(speakerMask, hasMap);
        auto client = kStereoClient;
        client.speakerMask = speakerMask;
        fake.queries.clear();
        fake.reads = 0U;

        if (hasMap)
        {
          fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
        }
        else
        {
          fake.reply(Property::OutputMap).optBytes.reset();
        }

        auto const planRes = configureCoreAudioChannels(fake.operations(), client, outputFormat());
        REQUIRE(planRes);
        CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{0U, 1U});
        CHECK(planRes->hasIdentityOrder);
        CHECK_FALSE(planRes->delivery.isMapped);
        CHECK(planRes->delivery.nativeSpeakerMask == (hasMap ? 0x3U : 0U));
        CHECK(planRes->outputDelivery.nativeSpeakerMask == 0x3U);
        CHECK_FALSE(planRes->delivery.hasSharedConversion);
        CHECK(fake.queries ==
              std::vector<Property>{Property::ClientLayout, Property::OutputLayout, Property::OutputMap});
        CHECK(fake.reads == (hasMap ? 2U : 1U));
        CHECK(fake.sets == 0U);
      }
    }
  }

  TEST_CASE("CoreAudioChannelLayout - unusable optional preferences do not veto legacy transport")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::DeviceLayout).optBytes = descriptions({1U, 2U});
    std::size_t expectedReads = 1U;

    SECTION("non-device-loss sizing failure")
    {
      fake.reply(Property::DeviceLayout).sizeStatus = ::kAudioHardwareIllegalOperationError;
      expectedReads = 0U;
    }

    SECTION("non-device-loss read failure")
    {
      fake.reply(Property::DeviceLayout).readStatus = ::kAudioHardwareIllegalOperationError;
    }

    SECTION("empty preference storage")
    {
      fake.reply(Property::DeviceLayout).optBytes = std::vector<std::byte>{};
      expectedReads = 0U;
    }

    SECTION("oversized preference storage")
    {
      fake.reply(Property::DeviceLayout).optBytes = std::vector<std::byte>(65537U);
      expectedReads = 0U;
    }

    SECTION("short successful preference read")
    {
      fake.reply(Property::DeviceLayout).shortRead = true;
    }

    SECTION("truncated preference header")
    {
      fake.reply(Property::DeviceLayout).optBytes = markers({0, 0, 0, 0});
    }

    SECTION("duplicate preference roles")
    {
      fake.reply(Property::DeviceLayout).optBytes = descriptions({1U, 1U});
    }

    auto const observationRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
    REQUIRE(observationRes);
    CHECK_FALSE(observationRes->optDeviceLabels);
    CHECK(fake.queries ==
          std::vector<Property>{
            Property::ClientLayout, Property::OutputLayout, Property::DeviceLayout, Property::OutputMap});
    CHECK(fake.reads == expectedReads);

    for (auto const speakerMask : {0U, 0x3U})
    {
      CAPTURE(speakerMask);
      auto client = kStereoClient;
      client.speakerMask = speakerMask;
      auto const planRes = planCoreAudioChannels(client, outputFormat(), *observationRes);
      REQUIRE(planRes);
      CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{0U, 1U});
      CHECK(planRes->hasIdentityOrder);
      CHECK(planRes->delivery.nativeSpeakerMask == 0U);
      CHECK(planRes->outputDelivery.nativeSpeakerMask == 0U);
      CHECK_FALSE(planRes->delivery.hasSharedConversion);
    }

    CHECK(fake.sets == 0U);
  }

  TEST_CASE("CoreAudioChannelLayout - optional preference handling never hides actual output readback faults")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    fake.reply(Property::DeviceLayout).sizeStatus = ::kAudioHardwareIllegalOperationError;
    auto property = Property::OutputLayout;
    bool hasSizeFailure = false;

    SECTION("current output sizing fails")
    {
      fake.reply(property).sizeStatus = ::kAudioHardwareIllegalOperationError;
      hasSizeFailure = true;
    }

    SECTION("current output reading fails")
    {
      fake.reply(property).readStatus = ::kAudioUnitErr_Uninitialized;
    }

    SECTION("current output read changes size")
    {
      fake.reply(property).shortRead = true;
    }

    SECTION("output map sizing fails")
    {
      property = Property::OutputMap;
      fake.reply(property).sizeStatus = ::kAudioHardwareIllegalOperationError;
      hasSizeFailure = true;
    }

    SECTION("output map reading fails")
    {
      property = Property::OutputMap;
      fake.reply(property).readStatus = ::kAudioUnitErr_Uninitialized;
    }

    SECTION("output map read changes size")
    {
      property = Property::OutputMap;
      fake.reply(property).shortRead = true;
    }

    for (auto const speakerMask : {0U, 0x3U})
    {
      CAPTURE(speakerMask);
      auto client = kStereoClient;
      client.speakerMask = speakerMask;
      fake.queries.clear();
      fake.reads = 0U;
      auto const planRes = configureCoreAudioChannels(fake.operations(), client, outputFormat());
      REQUIRE_FALSE(planRes);
      CHECK(planRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      auto const expectedQueries = property == Property::OutputLayout
                                     ? std::vector{Property::ClientLayout, Property::OutputLayout}
                                     : std::vector{Property::ClientLayout, Property::OutputLayout, Property::OutputMap};
      CHECK(fake.queries == expectedQueries);
      CHECK(fake.reads == (property == Property::OutputMap ? 1U : 0U) + (hasSizeFailure ? 0U : 1U));
      CHECK(fake.sets == 0U);
    }
  }

  TEST_CASE("CoreAudioChannelLayout - applicable preference device loss remains an output failure")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::DeviceLayout).optBytes = descriptions({1U, 2U});
    std::size_t expectedReads = 0U;

    SECTION("device lost during sizing")
    {
      fake.reply(Property::DeviceLayout).sizeStatus = ::kAudioHardwareBadDeviceError;
    }

    SECTION("device lost during reading")
    {
      fake.reply(Property::DeviceLayout).readStatus = ::kAudioHardwareBadObjectError;
      expectedReads = 1U;
    }

    auto const observationRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
    REQUIRE_FALSE(observationRes);
    CHECK(observationRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK(observationRes.error().error.code == Error::Code::DeviceNotFound);
    CHECK(fake.reads == expectedReads);
    CHECK(fake.queries ==
          std::vector<Property>{Property::ClientLayout, Property::OutputLayout, Property::DeviceLayout});
    CHECK(fake.sets == 0U);
  }

  TEST_CASE("CoreAudioChannelLayout - conflicting current output roles remain a refusal without a device preference")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::OutputLayout).optBytes = descriptions({3U, 2U});

    SECTION("default ordinal relationship")
    {
    }

    SECTION("observed identity map")
    {
      fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    }

    auto const planRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE_FALSE(planRes);
    CHECK(planRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    CHECK(planRes.error().error.code == Error::Code::FormatRejected);
    CHECK(fake.sets == 0U);
  }

  TEST_CASE(
    "CoreAudioChannelLayout - identity and same-set swaps preserve every sample storage byte before the native map")
  {
    struct MarkerCase final
    {
      SampleEncoding encoding;
      std::size_t sampleBytes;
      std::vector<std::byte> canonical;
      std::vector<std::byte> native;
    };
    auto const cases = std::array{
      MarkerCase{SampleEncoding::Signed16Le,
                 2U,
                 markers({0x11, 0x12, 0x21, 0x22, 0x31, 0x32, 0x41, 0x42}),
                 markers({0x21, 0x22, 0x11, 0x12, 0x41, 0x42, 0x31, 0x32})},
      MarkerCase{SampleEncoding::Signed24PackedLe,
                 3U,
                 markers({0x11, 0x12, 0x13, 0x21, 0x22, 0x23, 0x31, 0x32, 0x33, 0x41, 0x42, 0x43}),
                 markers({0x21, 0x22, 0x23, 0x11, 0x12, 0x13, 0x41, 0x42, 0x43, 0x31, 0x32, 0x33})},
      MarkerCase{SampleEncoding::Signed24In32Le,
                 4U,
                 markers({0x11, 0x12, 0x13, 0x80, 0x21, 0x22, 0x23, 0xFF, 0x31, 0x32, 0x33, 0, 0x41, 0x42, 0x43, 0}),
                 markers({0x21, 0x22, 0x23, 0xFF, 0x11, 0x12, 0x13, 0x80, 0x41, 0x42, 0x43, 0, 0x31, 0x32, 0x33, 0})},
      MarkerCase{
        SampleEncoding::Signed32Le,
        4U,
        markers({0x11, 0x12, 0x13, 0x14, 0x21, 0x22, 0x23, 0x24, 0x31, 0x32, 0x33, 0x34, 0x41, 0x42, 0x43, 0x44}),
        markers({0x21, 0x22, 0x23, 0x24, 0x11, 0x12, 0x13, 0x14, 0x41, 0x42, 0x43, 0x44, 0x31, 0x32, 0x33, 0x34})},
      MarkerCase{SampleEncoding::Float32Le,
                 4U,
                 markers({0, 0, 0x80, 0x3F, 0, 0, 0, 0xBF, 0, 0, 0x80, 0x3E, 0, 0, 0, 0xBE}),
                 markers({0, 0, 0, 0xBF, 0, 0, 0x80, 0x3F, 0, 0, 0, 0xBE, 0, 0, 0x80, 0x3E})}};
    auto fake = ChannelObservationFake{};
    bool isSwap = true;

    SECTION("identity retains canonical bytes")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({1U, 2U});
      isSwap = false;
    }

    SECTION("private permutation satisfies a swapped native client layout")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({2U, 1U});
    }

    SECTION("private permutation composes with a destination-to-source AU map")
    {
      fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
      fake.reply(Property::OutputMap).optBytes = ordinalMap({1, 0});
    }

    auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
    REQUIRE(observedRes);

    for (auto const& value : cases)
    {
      CAPTURE(value.encoding);
      auto client = kStereoClient;
      client.encoding = value.encoding;
      auto const planRes = planCoreAudioChannels(client, outputFormat(), *observedRes);
      REQUIRE(planRes);
      CHECK(planRes->sourceSlots == (isSwap ? std::vector<std::uint32_t>{1U, 0U} : std::vector<std::uint32_t>{0U, 1U}));
      CHECK(planRes->hasIdentityOrder == !isSwap);
      auto native = std::vector<std::byte>(value.canonical.size());
      copyCoreAudioChannels(native, value.canonical, value.sampleBytes, planRes->sourceSlots);
      CHECK(native == (isSwap ? value.native : value.canonical));

      if (observedRes->optOutputMap)
      {
        // Independent two-slot destination oracle, including intermediate bytes.
        auto destination = std::vector<std::byte>(native.size());

        for (std::size_t frame = 0U; frame < native.size(); frame += 2U * value.sampleBytes)
        {
          std::memcpy(destination.data() + frame, native.data() + frame + value.sampleBytes, value.sampleBytes);
          std::memcpy(destination.data() + frame + value.sampleBytes, native.data() + frame, value.sampleBytes);
        }

        CHECK(destination == value.canonical);
      }
    }
  }

  TEST_CASE("CoreAudioChannelLayout - identity null binding aliases render storage while permutations stay separate")
  {
    auto fake = ChannelObservationFake{};
    bool isIdentity = true;
    auto client = kStereoClient;

    SECTION("identified identity")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({1U, 2U});
    }

    SECTION("unidentified identity")
    {
      client.speakerMask = 0U;
    }

    SECTION("true permutation")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({2U, 1U});
      isIdentity = false;
    }

    auto const planRes = configureCoreAudioChannels(fake.operations(), client, outputFormat());
    REQUIRE(planRes);
    CHECK(planRes->hasIdentityOrder == isIdentity);
    auto staging = std::array<std::byte, 12>{};
    auto native = std::vector<std::byte>(planRes->hasIdentityOrder ? 0U : staging.size());
    auto const fallback = planRes->hasIdentityOrder ? std::span<std::byte>{staging} : std::span{native};
    auto buffers = ::AudioBufferList{
      .mNumberBuffers = 1U, .mBuffers = {{.mNumberChannels = 2U, .mDataByteSize = 12U, .mData = nullptr}}};
    auto const bound = bindCoreAudioRenderBuffer(&buffers, fallback, 12U, planRes->nativeClientFormat.channels);
    REQUIRE(bound.valid);
    CHECK(bound.output.size() == 12U);
    CHECK(buffers.mBuffers[0].mDataByteSize == 12U);
    CHECK((bound.output.data() == staging.data()) == isIdentity);
    CHECK(buffers.mBuffers[0].mData == (isIdentity ? staging.data() : native.data()));
    auto provided = std::array<std::byte, 12>{};
    buffers.mBuffers[0].mData = provided.data();
    auto const providedBound = bindCoreAudioRenderBuffer(&buffers, fallback, 12U, planRes->nativeClientFormat.channels);
    REQUIRE(providedBound.valid);
    CHECK(providedBound.output.data() == provided.data());
    CHECK(providedBound.output.size() == 12U);
  }

  TEST_CASE("CoreAudioChannelLayout - swapped native graph storage retains its observed set and canonical opened mode")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::ClientLayout).optBytes = descriptions({2U, 1U});
    auto const planRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(planRes);
    auto const opened = coreAudioOpenedPcmMode(kStereoClient, *planRes);
    CHECK(opened.clientFormat == kStereoClient);
    CHECK(opened.clientFormat.speakerMask == 0x3U);
    CHECK_FALSE(opened.optEndpoint);
    CHECK(opened.channelDelivery.nativeSpeakerMask == 0x3U);
    CHECK_FALSE(opened.channelDelivery.isMapped);
    auto const graph = coreAudioGraph({.routeAnchor = "native-swap",
                                       .optClientFormat = planRes->nativeClientFormat,
                                       .channelDelivery = planRes->delivery,
                                       .outputChannelDelivery = planRes->outputDelivery});
    REQUIRE(graph.nodes.size() == 2U);
    CHECK(graph.nodes[0].optFormat ==
          NodeFormat{PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed24PackedLe}});
    CHECK(graph.nodes[0].channelDelivery.nativeSpeakerMask == 0x3U);
    CHECK(graph.nodes[1].channelDelivery.nativeSpeakerMask == 0U);
    auto const canonical = markers({1, 2, 3, 4, 5, 6});
    auto native = std::vector<std::byte>(6U);
    copyCoreAudioChannels(native, canonical, 3U, planRes->sourceSlots);
    CHECK(native == markers({4, 5, 6, 1, 2, 3}));
  }

  TEST_CASE("CoreAudioChannelLayout - native client positions require observation rather than source identity")
  {
    auto fake = ChannelObservationFake{};
    std::uint32_t expectedNativeSet = 0U;
    std::uint32_t expectedStorageMask = 0U;
    auto client = kStereoClient;

    SECTION("missing roles cannot borrow the canonical source set")
    {
    }

    SECTION("opaque partial client roles are not completed from the source")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({1U, 0xFFFFFFFFU});
    }

    SECTION("an actually observed identity layout establishes canonical native storage")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({1U, 2U});
      expectedNativeSet = 0x3U;
      expectedStorageMask = 0x3U;
    }

    SECTION("native observations establish a set even when the application identity is unknown")
    {
      client.speakerMask = 0U;
      fake.reply(Property::ClientLayout).optBytes = descriptions({2U, 1U});
      expectedNativeSet = 0x3U;
    }

    SECTION("a read lossless ordinal map independently locates client roles")
    {
      fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
      fake.reply(Property::OutputMap).optBytes = ordinalMap({1, 0});
      expectedNativeSet = 0x3U;
    }

    auto const planRes = configureCoreAudioChannels(fake.operations(), client, outputFormat());
    REQUIRE(planRes);
    CHECK(planRes->delivery.nativeSpeakerMask == expectedNativeSet);
    CHECK(planRes->nativeClientFormat.speakerMask == expectedStorageMask);
    CHECK(coreAudioOpenedPcmMode(client, *planRes).clientFormat == client);
  }

  TEST_CASE(
    "CoreAudioChannelLayout - an observed nonbijective ordinal map preserves known conversion without inventing roles")
  {
    auto fake = ChannelObservationFake{};

    SECTION("a silent destination drops one canonical slot")
    {
      fake.reply(Property::OutputMap).optBytes = ordinalMap({-1, 1});
    }

    SECTION("a repeated source duplicates one slot and drops the other")
    {
      fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 0});
    }

    auto const planRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(planRes);
    CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{0U, 1U});
    CHECK(planRes->delivery.hasSharedConversion);
    CHECK_FALSE(planRes->delivery.isMapped);
    CHECK(planRes->delivery.nativeSpeakerMask == 0U);
    CHECK(fake.sets == 0U);
  }

  TEST_CASE("CoreAudioChannelLayout - same-set client and output differences configure a lossless ordinal relationship")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::ClientLayout).optBytes = descriptions({2U, 1U});
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    auto const planRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(planRes);
    CHECK(fake.sets == 1U);
    CHECK(fake.reply(Property::OutputMap).optBytes == ordinalMap({1, 0}));
    CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{1U, 0U});
    CHECK(planRes->nativeClientFormat.speakerMask == 0U);
    CHECK_FALSE(planRes->delivery.isMapped);
    CHECK(planRes->delivery.nativeSpeakerMask == 0x3U);
    CHECK(planRes->outputDelivery.nativeSpeakerMask == 0x3U);
    CHECK_FALSE(planRes->delivery.hasSharedConversion);
    auto const canonical = markers({1, 2, 3, 4, 5, 6});
    auto native = std::vector<std::byte>(6U);
    copyCoreAudioChannels(native, canonical, 3U, planRes->sourceSlots);
    CHECK(native == markers({4, 5, 6, 1, 2, 3}));
    // The readback map {1,0} sends native1 to left and native0 to right.
    CHECK(std::vector<std::byte>(native.begin() + 3, native.end()) == markers({1, 2, 3}));
    CHECK(std::vector<std::byte>(native.begin(), native.begin() + 3) == markers({4, 5, 6}));
  }

  TEST_CASE("CoreAudioChannelLayout - accepted map repair requires fresh actual AU roles for every mapped pair")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::ClientLayout).optBytes = descriptions({2U, 1U});
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    auto optFreshClient = std::optional<std::vector<std::byte>>{};
    auto optFreshOutput = std::optional<std::vector<std::byte>>{};
    bool hasProof = false;

    SECTION("both actual layouts become absent")
    {
    }

    SECTION("both actual layouts become opaque")
    {
      optFreshClient = descriptions({::kAudioChannelLabel_Unknown, ::kAudioChannelLabel_Unknown});
      optFreshOutput = descriptions({::kAudioChannelLabel_Unknown, ::kAudioChannelLabel_Unknown});
    }

    SECTION("absent client and opaque output have no role proof")
    {
      optFreshOutput = descriptions({::kAudioChannelLabel_Discrete_0, ::kAudioChannelLabel_Unused});
    }

    SECTION("partial observations leave the same map pair unproved")
    {
      optFreshClient = descriptions({2U, ::kAudioChannelLabel_Unknown});
      optFreshOutput = descriptions({::kAudioChannelLabel_Unknown, 2U});
    }

    SECTION("device preference alone cannot replace actual role proof")
    {
      fake.reply(Property::DeviceLayout).optBytes = descriptions({1U, 2U});
    }

    SECTION("fresh complete actual layouts retain the repaired byte order")
    {
      optFreshClient = descriptions({2U, 1U});
      optFreshOutput = descriptions({1U, 2U});
      hasProof = true;
    }

    SECTION("fresh client roles and the exact map prove the output order")
    {
      optFreshClient = descriptions({2U, 1U});
      hasProof = true;
    }

    SECTION("fresh output roles and the exact map prove the client order")
    {
      optFreshOutput = descriptions({1U, 2U});
      hasProof = true;
    }

    SECTION("complementary partial actual layouts prove every mapped pair")
    {
      optFreshClient = descriptions({2U, ::kAudioChannelLabel_Unknown});
      optFreshOutput = descriptions({1U, ::kAudioChannelLabel_Unknown});
      hasProof = true;
    }

    fake.afterRead = [&]
    {
      // Initial client/output/map bytes are already copied; change the next
      // observations before SET without borrowing their old role proof.
      if (fake.sets == 0U && fake.reads == 3U)
      {
        fake.reply(Property::ClientLayout).optBytes = optFreshClient;
        fake.reply(Property::OutputLayout).optBytes = optFreshOutput;
      }
    };
    auto const planRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    CHECK(fake.sets == 1U);
    CHECK(fake.reply(Property::OutputMap).optBytes == ordinalMap({1, 0}));

    if (!hasProof)
    {
      REQUIRE_FALSE(planRes);
      CHECK(planRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK(planRes.error().error.code == Error::Code::InitFailed);
      CHECK(planRes.error().error.message == "Core Audio accepted output channel map lacks fresh AU role proof");
    }
    else
    {
      REQUIRE(planRes);
      CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{1U, 0U});
      CHECK_FALSE(planRes->hasIdentityOrder);
      CHECK_FALSE(planRes->delivery.isMapped);
      auto native = std::vector<std::byte>(6U);
      copyCoreAudioChannels(native, markers({1, 2, 3, 4, 5, 6}), 3U, planRes->sourceSlots);
      CHECK(native == markers({4, 5, 6, 1, 2, 3}));
      // Accepted destination map {1,0} restores canonical left/right bytes.
      CHECK(std::vector<std::byte>(native.begin() + 3, native.end()) == markers({1, 2, 3}));
      CHECK(std::vector<std::byte>(native.begin(), native.begin() + 3) == markers({4, 5, 6}));
    }
  }

  TEST_CASE("CoreAudioChannelLayout - failed or divergent map configuration never erases known contradiction")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::ClientLayout).optBytes = descriptions({2U, 1U});
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    auto expectedCause = BackendOpenFailure::Cause::OutputFailure;

    SECTION("an unsupported map leaves the observed contradiction contextual")
    {
      fake.setStatus = ::kAudioUnitErr_InvalidProperty;
      expectedCause = BackendOpenFailure::Cause::TrackRouteIncompatible;
    }

    SECTION("an unused map property is a contextual capability refusal")
    {
      fake.setStatus = ::kAudioUnitErr_PropertyNotInUse;
      expectedCause = BackendOpenFailure::Cause::TrackRouteIncompatible;
    }

    SECTION("an explicit unsupported map format is a contextual capability refusal")
    {
      fake.setStatus = ::kAudioUnitErr_FormatNotSupported;
      expectedCause = BackendOpenFailure::Cause::TrackRouteIncompatible;
    }

    SECTION("a native device fault is terminal")
    {
      fake.setStatus = ::kAudioHardwareBadDeviceError;
    }

    SECTION("a native resource fault is terminal")
    {
      fake.setStatus = ::kAudio_MemFullError;
    }

    SECTION("an unknown native fault is terminal")
    {
      fake.setStatus = -123456;
    }

    SECTION("successful SET with missing readback is terminal")
    {
      fake.afterRead = [&]
      {
        if (fake.sets != 0U)
        {
          fake.reply(Property::OutputMap).optBytes.reset();
        }
      };
    }

    SECTION("successful SET with short readback is terminal")
    {
      fake.afterRead = [&]
      {
        if (fake.sets != 0U)
        {
          fake.reply(Property::OutputMap).shortRead = true;
        }
      };
    }

    SECTION("successful SET with faulting readback is terminal")
    {
      fake.afterRead = [&]
      {
        if (fake.sets != 0U)
        {
          fake.reply(Property::OutputMap).readStatus = ::kAudioUnitErr_Uninitialized;
        }
      };
    }

    SECTION("successful SET with unchanged readback is terminal")
    {
      fake.ignoreSet = true;
    }

    auto const planRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE_FALSE(planRes);
    CHECK(fake.sets == 1U);
    CHECK(planRes.error().cause == expectedCause);

    if (fake.setStatus != ::noErr)
    {
      CHECK(planRes.error().error.message.contains("Core Audio output channel"));
      CHECK(planRes.error().error.message.contains("OSStatus"));
      CHECK(planRes.error().error.location.line() != 0U);
    }
  }

  TEST_CASE(
    "CoreAudioChannelLayout - contradictory role readback after accepted map configuration is an output failure")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::ClientLayout).optBytes = descriptions({2U, 1U});
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    fake.afterRead = [&]
    {
      // Change output roles on the first post-SET read, leaving the map intact.
      if (fake.sets > 0U)
      {
        fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 3U});
      }
    };
    auto const planRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE_FALSE(planRes);
    CHECK(fake.sets == 1U);
    CHECK(fake.reads == 6U);
    CHECK(fake.reply(Property::OutputMap).optBytes == ordinalMap({1, 0}));
    CHECK(fake.reply(Property::OutputLayout).optBytes == descriptions({1U, 3U}));
    CHECK(planRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK(planRes.error().error.code == Error::Code::FormatRejected);
    CHECK(planRes.error().error.message ==
          "Core Audio observed channel roles cannot carry the canonical source losslessly");
  }

  TEST_CASE("CoreAudioChannelLayout - execution and worker refresh reject changed required channel relationships")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::ClientLayout).optBytes = descriptions({2U, 1U});
    auto const initialPlanRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(initialPlanRes);
    bool shouldRetain = false;

    SECTION("same relationship retains provisioned realtime storage")
    {
      shouldRetain = true;
    }

    SECTION("a changed native order requires reopening")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({1U, 2U});
    }

    SECTION("unavailable client roles cannot retain an obsolete nonidentity relationship")
    {
      fake.reply(Property::ClientLayout).optBytes.reset();
    }

    SECTION("a new known role contradiction is terminal after open")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({3U, 2U});
    }

    for (auto const mode : {CoreAudioChannelRefreshMode::Execution, CoreAudioChannelRefreshMode::Observation})
    {
      CAPTURE(mode);
      auto const refreshRes =
        refreshCoreAudioChannels(fake.operations(), kStereoClient, *initialPlanRes, mode, [] { return true; });

      if (shouldRetain)
      {
        REQUIRE(refreshRes);
        REQUIRE(*refreshRes);
        CHECK(canRetainCoreAudioChannelPlan(*initialPlanRes, (**refreshRes).plan));
        CHECK((**refreshRes).plan.nativeClientFormat.speakerMask == 0U);
        CHECK((**refreshRes).plan.delivery.nativeSpeakerMask == 0x3U);
      }
      else
      {
        REQUIRE_FALSE(refreshRes);
        CHECK(refreshRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      }

      CHECK(fake.sets == 0U);
      CHECK(initialPlanRes->sourceSlots == std::vector<std::uint32_t>{1U, 0U});
      CHECK_FALSE(initialPlanRes->hasIdentityOrder);
    }
  }

  TEST_CASE("CoreAudioChannelLayout - execution and worker refresh require fresh exact input storage")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    auto const configuredRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(configuredRes);

    SECTION("input getter fails")
    {
      fake.inputStatus = ::kAudioUnitErr_Uninitialized;
    }

    SECTION("input readback size is invalid")
    {
      fake.inputFormatBytes = static_cast<::UInt32>(sizeof(::AudioStreamBasicDescription)) - 1U;
    }

    SECTION("successful input readback has no storage")
    {
      fake.inputFormatBytes = 0U;
    }

    SECTION("successful input readback exceeds the ASBD extent")
    {
      fake.inputFormatBytes = static_cast<::UInt32>(sizeof(::AudioStreamBasicDescription)) + 1U;
    }

    SECTION("a real native input size error retains its status instead of becoming malformed success")
    {
      fake.inputStatus = ::kAudioHardwareBadPropertySizeError;
      fake.inputFormatBytes = 0U;
    }

    SECTION("input sample rate changed")
    {
      fake.inputFormat.mSampleRate = 44100.0;
    }

    SECTION("input channel count changed")
    {
      fake.inputFormat.mChannelsPerFrame = 1U;
    }

    SECTION("input frame storage changed")
    {
      fake.inputFormat.mBytesPerFrame = 8U;
    }

    SECTION("input interleaving changed")
    {
      fake.inputFormat.mFormatFlags |= ::kAudioFormatFlagIsNonInterleaved;
    }

    SECTION("input precision changed")
    {
      fake.inputFormat.mBitsPerChannel = 16U;
    }

    for (auto const mode : {CoreAudioChannelRefreshMode::Execution, CoreAudioChannelRefreshMode::Observation})
    {
      CAPTURE(mode);
      fake.queries.clear();
      fake.inputFormatReads = 0U;
      fake.outputFormatReads = 0U;
      auto const refreshRes =
        refreshCoreAudioChannels(fake.operations(), kStereoClient, *configuredRes, mode, [] { return true; });
      REQUIRE_FALSE(refreshRes);
      CHECK(refreshRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK(fake.inputFormatReads == 1U);
      CHECK(fake.inputFormatCapacity == sizeof(::AudioStreamBasicDescription));
      CHECK(fake.outputFormatReads == 0U);
      CHECK(fake.queries.empty());
      CHECK(refreshRes.error().error.location.line() != 0U);

      if (fake.inputStatus != ::noErr)
      {
        CHECK(refreshRes.error().error.message.contains("read Core Audio current client format failed (OSStatus"));
        CHECK_FALSE(refreshRes.error().error.message.contains("invalid ASBD byte extent"));
      }
      else if (fake.inputFormatBytes != sizeof(::AudioStreamBasicDescription))
      {
        CHECK(refreshRes.error().error.message.contains(
          "read Core Audio current client format returned an invalid ASBD byte extent"));
        CHECK(refreshRes.error().error.message.contains(std::format("returnedBytes={}", fake.inputFormatBytes)));
      }

      CHECK(configuredRes->sourceSlots == std::vector<std::uint32_t>{0U, 1U});
    }
  }

  TEST_CASE("CoreAudioChannelLayout - fresh output geometry separates optional diagnostics from execution")
  {
    auto fake = ChannelObservationFake{};
    std::uint32_t expectedOutputMask = 0U;

    SECTION("current output layout independently proves destination count")
    {
      fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
      expectedOutputMask = 0x3U;
    }

    SECTION("current ordinal map independently proves destination count")
    {
      fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    }

    // Failed getter bytes are never numerical evidence or geometry.
    fake.outputStatus = ::kAudioUnitErr_Uninitialized;
    fake.downstreamFormat = outputFormat(6U);
    fake.downstreamFormat.mSampleRate = 96000.0;

    for (auto const speakerMask : {0U, 0x3U})
    {
      auto client = kStereoClient;
      client.speakerMask = speakerMask;
      auto const configuredRes = configureCoreAudioChannels(fake.operations(), client, outputFormat());
      REQUIRE(configuredRes);

      for (auto const mode : {CoreAudioChannelRefreshMode::Execution, CoreAudioChannelRefreshMode::Observation})
      {
        CAPTURE(speakerMask, mode);
        fake.queries.clear();
        fake.inputFormatReads = 0U;
        fake.outputFormatReads = 0U;
        auto const refreshRes =
          refreshCoreAudioChannels(fake.operations(), client, *configuredRes, mode, [] { return true; });
        REQUIRE(refreshRes);
        REQUIRE(*refreshRes);
        auto const& refresh = **refreshRes;
        CHECK_FALSE(refresh.optOutputFormat);
        CHECK(refresh.plan.sourceSlots == std::vector<std::uint32_t>{0U, 1U});
        CHECK(refresh.plan.hasIdentityOrder);
        CHECK(refresh.plan.outputDelivery.nativeSpeakerMask == expectedOutputMask);
        CHECK_FALSE(refresh.plan.outputDelivery.hasSharedConversion);
        CHECK(fake.inputFormatReads == 1U);
        CHECK(fake.outputFormatReads == (mode == CoreAudioChannelRefreshMode::Observation ? 1U : 0U));
        CHECK(fake.sets == 0U);
      }
    }
  }

  TEST_CASE("CoreAudioChannelLayout - optional downstream failure retains only independently proved transport")
  {
    auto fake = ChannelObservationFake{};
    auto client = kStereoClient;
    auto expectedSlots = std::vector<std::uint32_t>{1U, 0U};
    bool hasIdentityOrder = false;
    bool hasSharedConversion = false;
    bool shouldDropFallback = false;
    std::uint32_t expectedNativeMask = 0x3U;
    std::uint32_t expectedOutputMask = 0x3U;

    SECTION("centered mono retains opaque Mono through a fresh two-destination map")
    {
      client.channels = 1U;
      client.speakerMask = 0x4U;
      fake.reply(Property::ClientLayout).optBytes = descriptions({::kAudioChannelLabel_Mono});
      fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 0});
      expectedSlots = {0U};
      hasIdentityOrder = true;
      hasSharedConversion = true;
      expectedNativeMask = 0U;
      expectedOutputMask = 0U;
    }

    SECTION("a real nonidentity permutation retains fresh client output and map proof")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({2U, 1U});
      fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
      fake.reply(Property::OutputMap).optBytes = ordinalMap({1, 0});
    }

    SECTION("a preference-derived permutation uses fresh map count and applicable fallback")
    {
      fake.reply(Property::DeviceLayout).optBytes = descriptions({2U, 1U});
      fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    }

    SECTION("fresh map count cannot replace an unavailable required preference")
    {
      fake.reply(Property::DeviceLayout).optBytes = descriptions({2U, 1U});
      fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
      shouldDropFallback = true;
    }

    fake.inputFormat = outputFormat(client.channels);
    auto const configuredRes = configureCoreAudioChannels(fake.operations(), client, outputFormat());
    REQUIRE(configuredRes);
    REQUIRE(configuredRes->sourceSlots == expectedSlots);
    REQUIRE(configuredRes->hasIdentityOrder == hasIdentityOrder);

    if (shouldDropFallback)
    {
      fake.reply(Property::DeviceLayout).optBytes.reset();
    }

    // Only the fresh map/layout, not failed getter bytes, can prove count.
    fake.outputStatus = ::kAudioUnitErr_Uninitialized;
    fake.downstreamFormat = outputFormat(6U);
    fake.downstreamFormat.mSampleRate = 96000.0;

    for (auto const mode : {CoreAudioChannelRefreshMode::Execution, CoreAudioChannelRefreshMode::Observation})
    {
      CAPTURE(mode);
      fake.inputFormatReads = 0U;
      fake.outputFormatReads = 0U;
      auto const refreshRes =
        refreshCoreAudioChannels(fake.operations(), client, *configuredRes, mode, [] { return true; });
      CHECK(fake.inputFormatReads == 1U);
      CHECK(fake.outputFormatReads == (mode == CoreAudioChannelRefreshMode::Observation ? 1U : 0U));

      if (shouldDropFallback)
      {
        REQUIRE_FALSE(refreshRes);
        CHECK(refreshRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
        CHECK(refreshRes.error().error.message == "Core Audio current channel plan requires different render slots");
      }
      else
      {
        REQUIRE(refreshRes);
        REQUIRE(*refreshRes);
        auto const& refreshed = **refreshRes;
        CHECK_FALSE(refreshed.optOutputFormat);
        CHECK(refreshed.plan.sourceSlots == expectedSlots);
        CHECK(refreshed.plan.hasIdentityOrder == hasIdentityOrder);
        CHECK(refreshed.plan.nativeClientFormat.sampleRate == 48000U);
        CHECK(refreshed.plan.nativeClientFormat.channels == client.channels);
        CHECK(refreshed.plan.nativeClientFormat.speakerMask == 0U);
        CHECK(refreshed.plan.delivery.nativeSpeakerMask == expectedNativeMask);
        CHECK(refreshed.plan.outputDelivery.nativeSpeakerMask == expectedOutputMask);
        CHECK(refreshed.plan.delivery.hasSharedConversion == hasSharedConversion);
        CHECK(refreshed.plan.outputDelivery.hasSharedConversion == hasSharedConversion);
      }

      CHECK(configuredRes->sourceSlots == expectedSlots);
      CHECK(configuredRes->hasIdentityOrder == hasIdentityOrder);
      CHECK(fake.sets == 0U);
    }
  }

  TEST_CASE("CoreAudioChannelLayout - current native geometry survives an unavailable diagnostic projection")
  {
    auto output = outputFormat();

    SECTION("a positive fractional native rate remains a fresh observation")
    {
      output.mSampleRate = 48000.5;
    }

    SECTION("a wider native destination count remains a fresh observation")
    {
      output = outputFormat(256U);
    }

    REQUIRE_FALSE(coreAudioSignalFormat(output));

    for (auto const speakerMask : {0U, 0x3U})
    {
      auto fake = ChannelObservationFake{};
      auto client = kStereoClient;
      client.speakerMask = speakerMask;
      auto const configuredRes = configureCoreAudioChannels(fake.operations(), client, outputFormat());
      REQUIRE(configuredRes);
      fake.downstreamFormat = output;

      for (auto const mode : {CoreAudioChannelRefreshMode::Execution, CoreAudioChannelRefreshMode::Observation})
      {
        CAPTURE(speakerMask, mode, output.mSampleRate, output.mChannelsPerFrame);
        fake.inputFormatReads = 0U;
        fake.outputFormatReads = 0U;
        auto const refreshRes =
          refreshCoreAudioChannels(fake.operations(), client, *configuredRes, mode, [] { return true; });
        REQUIRE(refreshRes);
        REQUIRE(*refreshRes);
        auto const& refreshed = **refreshRes;
        REQUIRE(refreshed.optOutputFormat);
        CHECK(refreshed.optOutputFormat->mSampleRate == output.mSampleRate);
        CHECK(refreshed.optOutputFormat->mChannelsPerFrame == output.mChannelsPerFrame);
        CHECK_FALSE(coreAudioSignalFormat(*refreshed.optOutputFormat));
        CHECK(refreshed.plan.sourceSlots == std::vector<std::uint32_t>{0U, 1U});
        CHECK(refreshed.plan.hasIdentityOrder);
        CHECK(refreshed.plan.nativeClientFormat == configuredRes->nativeClientFormat);
        CHECK_FALSE(refreshed.plan.delivery.isMapped);
        CHECK(refreshed.plan.outputDelivery.nativeSpeakerMask == 0U);
        CHECK(refreshed.plan.outputDelivery.hasSharedConversion == (output.mChannelsPerFrame == 256U));
        CHECK(fake.inputFormatReads == 1U);
        CHECK(fake.outputFormatReads == 1U);
        CHECK(fake.sets == 0U);
      }
    }
  }

  TEST_CASE("CoreAudioChannelLayout - independent geometry does not hide downstream device loss or malformed success")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    auto const configuredRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(configuredRes);
    auto expectedCode = Error::Code::InitFailed;

    SECTION("downstream getter reports device loss")
    {
      fake.outputStatus = ::kAudioHardwareBadDeviceError;
      expectedCode = Error::Code::DeviceNotFound;
    }

    SECTION("downstream getter reports a stopped device")
    {
      fake.outputStatus = ::kAudioHardwareNotRunningError;
      expectedCode = Error::Code::DeviceNotFound;
    }

    SECTION("successful downstream getter returns invalid numerical facts")
    {
      fake.downstreamFormat.mChannelsPerFrame = 0U;
    }

    SECTION("successful downstream getter returns a zero rate")
    {
      fake.downstreamFormat.mSampleRate = 0.0;
    }

    SECTION("successful downstream getter returns a negative rate")
    {
      fake.downstreamFormat.mSampleRate = -48000.0;
    }

    SECTION("successful downstream getter returns a nonfinite rate")
    {
      fake.downstreamFormat.mSampleRate = std::numeric_limits<double>::infinity();
    }

    SECTION("successful downstream getter returns a not a number rate")
    {
      fake.downstreamFormat.mSampleRate = std::numeric_limits<double>::quiet_NaN();
    }

    auto const refreshRes = refreshCoreAudioChannels(
      fake.operations(), kStereoClient, *configuredRes, CoreAudioChannelRefreshMode::Observation, [] { return true; });
    REQUIRE_FALSE(refreshRes);
    CHECK(refreshRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK(refreshRes.error().error.code == expectedCode);
    CHECK(fake.inputFormatReads == 1U);
    CHECK(fake.outputFormatReads == 1U);
  }

  TEST_CASE("CoreAudioChannelLayout - queried downstream success requires exact ASBD storage independently of geometry")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    auto const configuredRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(configuredRes);

    SECTION("successful downstream read returns zero bytes")
    {
      fake.outputFormatBytes = 0U;
    }

    SECTION("successful downstream read returns short storage")
    {
      fake.outputFormatBytes = static_cast<::UInt32>(sizeof(::AudioStreamBasicDescription)) - 1U;
    }

    SECTION("successful downstream read returns excess storage")
    {
      fake.outputFormatBytes = static_cast<::UInt32>(sizeof(::AudioStreamBasicDescription)) + 1U;
    }

    for (auto const hasActualGeometry : {true, false})
    {
      if (!hasActualGeometry)
      {
        fake.reply(Property::OutputLayout).optBytes.reset();
        fake.reply(Property::OutputMap).optBytes.reset();
      }

      for (auto const mode : {CoreAudioChannelRefreshMode::Observation, CoreAudioChannelRefreshMode::Execution})
      {
        CAPTURE(hasActualGeometry, mode, fake.outputFormatBytes);
        fake.inputFormatReads = 0U;
        fake.outputFormatReads = 0U;
        fake.queries.clear();
        auto const refreshRes =
          refreshCoreAudioChannels(fake.operations(), kStereoClient, *configuredRes, mode, [] { return true; });
        CHECK(fake.inputFormatReads == 1U);
        CHECK(fake.inputFormatCapacity == sizeof(::AudioStreamBasicDescription));
        CHECK(fake.inputFormatBytes == sizeof(::AudioStreamBasicDescription));

        if (mode == CoreAudioChannelRefreshMode::Execution && hasActualGeometry)
        {
          // This getter is dispensable, so even its malformed hypothetical
          // reply is not an execution-admission observation.
          REQUIRE(refreshRes);
          REQUIRE(*refreshRes);
          CHECK_FALSE((**refreshRes).optOutputFormat);
          CHECK((**refreshRes).plan.sourceSlots == std::vector<std::uint32_t>{0U, 1U});
          CHECK(fake.outputFormatReads == 0U);
        }
        else
        {
          REQUIRE_FALSE(refreshRes);
          CHECK(refreshRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
          CHECK(refreshRes.error().error.code == Error::Code::InitFailed);
          CHECK(
            refreshRes.error().error.message ==
            std::format(
              "read Core Audio output format returned an invalid ASBD byte extent: expectedBytes={} returnedBytes={}",
              sizeof(::AudioStreamBasicDescription),
              fake.outputFormatBytes));
          CHECK(
            std::string_view{refreshRes.error().error.location.file_name()}.ends_with("CoreAudioChannelLayout.cpp"));
          CHECK(std::string_view{refreshRes.error().error.location.function_name()}.contains("readDownstreamFormat"));
          CHECK(refreshRes.error().error.location.line() != 0U);
          CHECK(fake.outputFormatReads == 1U);
          CHECK(fake.outputFormatCapacity == sizeof(::AudioStreamBasicDescription));

          if (mode == CoreAudioChannelRefreshMode::Observation)
          {
            CHECK(fake.queries.empty());
          }
        }

        CHECK(fake.sets == 0U);
        CHECK(configuredRes->sourceSlots == std::vector<std::uint32_t>{0U, 1U});
      }
    }
  }

  TEST_CASE("CoreAudioChannelLayout - raw downstream errors are not malformed successful ASBD extents")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    auto const configuredRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(configuredRes);
    fake.outputFormatBytes = 0U;
    fake.downstreamFormat.mChannelsPerFrame = 0U;
    bool isDeviceLost = false;

    SECTION("a real native bad-size error withholds optional numerical evidence")
    {
      fake.outputStatus = ::kAudioHardwareBadPropertySizeError;
    }

    SECTION("an ordinary native unavailable error withholds optional numerical evidence")
    {
      fake.outputStatus = ::kAudioUnitErr_Uninitialized;
    }

    SECTION("a raw device-loss status remains terminal before extent or format interpretation")
    {
      fake.outputStatus = ::kAudioHardwareBadDeviceError;
      isDeviceLost = true;
    }

    // Keep configure observations separate from the refresh-only checks.
    REQUIRE(fake.queries == std::vector<Property>{Property::ClientLayout, Property::OutputLayout, Property::OutputMap});
    fake.queries.clear();

    auto const refreshRes = refreshCoreAudioChannels(
      fake.operations(), kStereoClient, *configuredRes, CoreAudioChannelRefreshMode::Observation, [] { return true; });
    CHECK(fake.inputFormatReads == 1U);
    CHECK(fake.outputFormatReads == 1U);
    CHECK(fake.outputFormatCapacity == sizeof(::AudioStreamBasicDescription));

    if (isDeviceLost)
    {
      REQUIRE_FALSE(refreshRes);
      CHECK(refreshRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK(refreshRes.error().error.code == Error::Code::DeviceNotFound);
      CHECK(refreshRes.error().error.message.contains("read Core Audio output format failed (OSStatus '!dev'"));
      CHECK_FALSE(refreshRes.error().error.message.contains("invalid ASBD byte extent"));
      CHECK(fake.queries.empty());
    }
    else
    {
      REQUIRE(refreshRes);
      REQUIRE(*refreshRes);
      CHECK_FALSE((**refreshRes).optOutputFormat);
      CHECK((**refreshRes).plan.sourceSlots == std::vector<std::uint32_t>{0U, 1U});
      CHECK((**refreshRes).plan.outputDelivery.nativeSpeakerMask == 0x3U);
      CHECK(fake.queries == std::vector<Property>{Property::ClientLayout, Property::OutputLayout, Property::OutputMap});
    }
  }

  TEST_CASE("CoreAudioChannelLayout - optional downstream failure cannot hide required AU readback faults")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    auto const configuredRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(configuredRes);
    fake.outputStatus = ::kAudioUnitErr_Uninitialized;

    SECTION("actual client layout getter fails")
    {
      fake.reply(Property::ClientLayout).sizeStatus = ::kAudioHardwareIllegalOperationError;
    }

    SECTION("actual output layout is malformed")
    {
      fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 1U});
    }

    SECTION("actual map getter fails")
    {
      fake.reply(Property::OutputMap).readStatus = ::kAudioUnitErr_Uninitialized;
    }

    SECTION("actual map is malformed")
    {
      fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 2});
    }

    for (auto const mode : {CoreAudioChannelRefreshMode::Execution, CoreAudioChannelRefreshMode::Observation})
    {
      CAPTURE(mode);
      auto const refreshRes =
        refreshCoreAudioChannels(fake.operations(), kStereoClient, *configuredRes, mode, [] { return true; });
      REQUIRE_FALSE(refreshRes);
      CHECK(refreshRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK(fake.sets == 0U);
    }
  }

  TEST_CASE("CoreAudioChannelLayout - client count and preferences cannot replace missing current output geometry")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::DeviceLayout).optBytes = descriptions({1U, 2U});
    fake.outputStatus = ::kAudioUnitErr_Uninitialized;

    for (auto const speakerMask : {0U, 0x3U})
    {
      auto client = kStereoClient;
      client.speakerMask = speakerMask;
      auto const configuredRes = configureCoreAudioChannels(fake.operations(), client, outputFormat());
      REQUIRE(configuredRes);

      for (auto const mode : {CoreAudioChannelRefreshMode::Execution, CoreAudioChannelRefreshMode::Observation})
      {
        CAPTURE(speakerMask, mode);
        fake.queries.clear();
        fake.inputFormatReads = 0U;
        fake.outputFormatReads = 0U;
        auto const refreshRes =
          refreshCoreAudioChannels(fake.operations(), client, *configuredRes, mode, [] { return true; });
        REQUIRE_FALSE(refreshRes);
        CHECK(refreshRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
        CHECK(refreshRes.error().error.message == "Core Audio current output geometry could not be proved");
        CHECK(fake.inputFormatReads == 1U);
        CHECK(fake.outputFormatReads == 1U);
        CHECK(fake.queries ==
              std::vector<Property>{Property::ClientLayout, Property::OutputLayout, Property::OutputMap});
      }
    }
  }

  TEST_CASE("CoreAudioChannelLayout - fresh output changes cannot mutate provisioned source slots")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    auto const configuredRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(configuredRes);

    SECTION("current output order changes")
    {
      fake.reply(Property::OutputLayout).optBytes = descriptions({2U, 1U});
    }

    SECTION("current map changes")
    {
      fake.reply(Property::OutputMap).optBytes = ordinalMap({1, 0});
    }

    SECTION("current output and map counts disagree")
    {
      fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1, -1});
    }

    for (auto const mode : {CoreAudioChannelRefreshMode::Execution, CoreAudioChannelRefreshMode::Observation})
    {
      CAPTURE(mode);
      auto const refreshRes =
        refreshCoreAudioChannels(fake.operations(), kStereoClient, *configuredRes, mode, [] { return true; });
      REQUIRE_FALSE(refreshRes);
      CHECK(refreshRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK(configuredRes->sourceSlots == std::vector<std::uint32_t>{0U, 1U});
      CHECK(configuredRes->hasIdentityOrder);
      CHECK(fake.sets == 0U);
    }
  }

  TEST_CASE("CoreAudioChannelLayout - preference-derived permutations require a fresh applicable fallback")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::DeviceLayout).optBytes = descriptions({2U, 1U});
    fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    auto const configuredRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(configuredRes);
    REQUIRE_FALSE(configuredRes->hasIdentityOrder);
    bool shouldRetain = false;

    SECTION("applicable fallback is freshly confirmed")
    {
      shouldRetain = true;
    }

    SECTION("fallback becomes unavailable")
    {
      fake.reply(Property::DeviceLayout).optBytes.reset();
    }

    SECTION("fallback becomes malformed")
    {
      fake.reply(Property::DeviceLayout).optBytes = descriptions({1U, 1U});
    }

    SECTION("fallback no longer matches the current output count")
    {
      fake.reply(Property::DeviceLayout).optBytes = descriptions({2U, 1U, 3U});
    }

    for (auto const mode : {CoreAudioChannelRefreshMode::Execution, CoreAudioChannelRefreshMode::Observation})
    {
      CAPTURE(mode);
      fake.queries.clear();
      auto const refreshRes =
        refreshCoreAudioChannels(fake.operations(), kStereoClient, *configuredRes, mode, [] { return true; });
      CHECK(fake.queries ==
            std::vector<Property>{
              Property::ClientLayout, Property::OutputLayout, Property::OutputMap, Property::DeviceLayout});

      if (shouldRetain)
      {
        REQUIRE(refreshRes);
        REQUIRE(*refreshRes);
        CHECK((**refreshRes).plan.sourceSlots == std::vector<std::uint32_t>{1U, 0U});
        CHECK_FALSE((**refreshRes).plan.hasIdentityOrder);
        CHECK((**refreshRes).plan.outputDelivery.nativeSpeakerMask == 0x3U);
      }
      else
      {
        REQUIRE_FALSE(refreshRes);
        CHECK(refreshRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      }
    }
  }

  TEST_CASE("CoreAudioChannelLayout - execution defers dispensable preference and numerical observations")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::ClientLayout).optBytes = descriptions({2U, 1U});
    fake.reply(Property::OutputMap).optBytes = ordinalMap({0, 1});
    auto const configuredRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(configuredRes);
    fake.reply(Property::DeviceLayout).optBytes = descriptions({2U, 1U});

    for (auto const mode : {CoreAudioChannelRefreshMode::Execution, CoreAudioChannelRefreshMode::Observation})
    {
      CAPTURE(mode);
      fake.queries.clear();
      fake.inputFormatReads = 0U;
      fake.outputFormatReads = 0U;
      auto const refreshRes =
        refreshCoreAudioChannels(fake.operations(), kStereoClient, *configuredRes, mode, [] { return true; });
      REQUIRE(refreshRes);
      REQUIRE(*refreshRes);
      CHECK((**refreshRes).plan.sourceSlots == std::vector<std::uint32_t>{1U, 0U});
      CHECK((**refreshRes).plan.delivery.nativeSpeakerMask == 0x3U);
      CHECK((**refreshRes).optOutputFormat.has_value() == (mode == CoreAudioChannelRefreshMode::Observation));
      CHECK(fake.inputFormatReads == 1U);
      CHECK(fake.outputFormatReads == (mode == CoreAudioChannelRefreshMode::Observation ? 1U : 0U));
      auto expectedQueries = std::vector{Property::ClientLayout, Property::OutputLayout, Property::OutputMap};

      if (mode == CoreAudioChannelRefreshMode::Observation)
      {
        expectedQueries.push_back(Property::DeviceLayout);
        REQUIRE((**refreshRes).optOutputFormat);
        CHECK((**refreshRes).optOutputFormat->mSampleRate == 48000.0);
        CHECK((**refreshRes).optOutputFormat->mChannelsPerFrame == 2U);
        CHECK((**refreshRes).plan.outputDelivery.nativeSpeakerMask == 0x3U);
      }
      else
      {
        CHECK((**refreshRes).plan.outputDelivery.nativeSpeakerMask == 0U);
      }

      CHECK(fake.queries == expectedQueries);
    }
  }

  TEST_CASE("CoreAudioChannelLayout - unreadable identity refresh clears native sets without borrowing source roles")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::ClientLayout).optBytes = descriptions({1U, 2U});
    fake.reply(Property::OutputLayout).optBytes = descriptions({1U, 2U});
    auto const initialPlanRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(initialPlanRes);
    CHECK(initialPlanRes->nativeClientFormat.speakerMask == 0x3U);
    CHECK(initialPlanRes->delivery.nativeSpeakerMask == 0x3U);
    CHECK(initialPlanRes->outputDelivery.nativeSpeakerMask == 0x3U);
    fake.reply(Property::ClientLayout).optBytes.reset();
    fake.reply(Property::OutputLayout).optBytes.reset();
    auto const refreshRes = refreshCoreAudioChannels(
      fake.operations(), kStereoClient, *initialPlanRes, CoreAudioChannelRefreshMode::Observation, [] { return true; });
    REQUIRE(refreshRes);
    REQUIRE(*refreshRes);
    auto const& refreshedPlan = (**refreshRes).plan;
    CHECK(canRetainCoreAudioChannelPlan(*initialPlanRes, refreshedPlan));
    CHECK(refreshedPlan.nativeClientFormat.speakerMask == 0U);
    CHECK(refreshedPlan.delivery.nativeSpeakerMask == 0U);
    CHECK(refreshedPlan.outputDelivery.nativeSpeakerMask == 0U);
    auto const graph = coreAudioGraph({.routeAnchor = "refreshed",
                                       .optClientFormat = refreshedPlan.nativeClientFormat,
                                       .channelDelivery = refreshedPlan.delivery,
                                       .outputChannelDelivery = refreshedPlan.outputDelivery});
    REQUIRE(graph.nodes.size() == 2U);
    CHECK(graph.nodes[0].channelDelivery.nativeSpeakerMask == 0U);
    CHECK(graph.nodes[1].channelDelivery.nativeSpeakerMask == 0U);
    CHECK(coreAudioOpenedPcmMode(kStereoClient, refreshedPlan).clientFormat == kStereoClient);
  }

  TEST_CASE("CoreAudioChannelLayout - generation retirement and close discard native refreshes", "[concurrency]")
  {
    auto fake = ChannelObservationFake{};
    fake.reply(Property::ClientLayout).optBytes = descriptions({1U, 2U});
    auto const configuredRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(configuredRes);
    fake.reads = 0U;
    fake.queries.clear();
    auto generation = std::atomic<std::uint64_t>{7U};
    bool unitPresent = true;
    auto const isCurrent = [&] { return unitPresent && generation.load(std::memory_order_acquire) == 7U; };

    SECTION("obsolete queued generation performs no read")
    {
      generation.store(8U, std::memory_order_release);
    }

    SECTION("closed owner performs no read")
    {
      unitPresent = false;
    }

    SECTION("retirement during channel read discards the completed observation")
    {
      fake.afterRead = [&] { generation.store(8U, std::memory_order_release); };
    }

    SECTION("retirement during successful input read discards execution proof")
    {
      fake.afterInputRead = [&] { generation.store(8U, std::memory_order_release); };
    }

    SECTION("retirement during failed input read discards the obsolete fault")
    {
      fake.inputStatus = ::kAudioUnitErr_Uninitialized;
      fake.afterInputRead = [&] { generation.store(8U, std::memory_order_release); };
    }

    SECTION("retirement during downstream read discards diagnostics")
    {
      fake.afterOutputRead = [&] { generation.store(8U, std::memory_order_release); };
    }

    auto const queuedGeneration = generation.load(std::memory_order_acquire);

    for (auto const mode : {CoreAudioChannelRefreshMode::Execution, CoreAudioChannelRefreshMode::Observation})
    {
      CAPTURE(mode);
      generation.store(queuedGeneration, std::memory_order_release);
      fake.reads = 0U;
      fake.inputFormatReads = 0U;
      fake.outputFormatReads = 0U;
      auto const refreshRes =
        refreshCoreAudioChannels(fake.operations(), kStereoClient, *configuredRes, mode, isCurrent);
      REQUIRE(refreshRes);
      CHECK_FALSE(*refreshRes);
      bool const wasAdmitted = fake.afterRead || fake.afterInputRead || fake.afterOutputRead;
      CHECK(fake.inputFormatReads == (wasAdmitted ? 1U : 0U));
      CHECK(fake.outputFormatReads == (wasAdmitted && fake.inputStatus == ::noErr ? 1U : 0U));
      CHECK(fake.reads == (wasAdmitted && fake.inputStatus == ::noErr ? 1U : 0U));
    }
  }

  TEST_CASE("CoreAudioChannelLayout - retirement discards malformed successful ASBD storage failures", "[concurrency]")
  {
    auto fake = ChannelObservationFake{};
    auto const configuredRes = configureCoreAudioChannels(fake.operations(), kStereoClient, outputFormat());
    REQUIRE(configuredRes);
    auto generation = std::atomic<std::uint64_t>{7U};
    bool isInputFailure = false;

    SECTION("retirement during a short successful input read discards its failure")
    {
      isInputFailure = true;
      fake.inputFormatBytes = static_cast<::UInt32>(sizeof(::AudioStreamBasicDescription)) - 1U;
      fake.afterInputRead = [&] { generation.store(8U, std::memory_order_release); };
    }

    SECTION("retirement during a short successful downstream read discards its failure")
    {
      fake.outputFormatBytes = static_cast<::UInt32>(sizeof(::AudioStreamBasicDescription)) - 1U;
      fake.afterOutputRead = [&] { generation.store(8U, std::memory_order_release); };
    }

    for (auto const mode : {CoreAudioChannelRefreshMode::Execution, CoreAudioChannelRefreshMode::Observation})
    {
      CAPTURE(mode);
      generation.store(7U, std::memory_order_release);
      fake.inputFormatReads = 0U;
      fake.outputFormatReads = 0U;
      auto const refreshRes =
        refreshCoreAudioChannels(fake.operations(),
                                 kStereoClient,
                                 *configuredRes,
                                 mode,
                                 [&] { return generation.load(std::memory_order_acquire) == 7U; });
      REQUIRE(refreshRes);
      CHECK_FALSE(*refreshRes);
      CHECK(fake.inputFormatReads == 1U);
      CHECK(fake.outputFormatReads == (isInputFailure ? 0U : 1U));
      CHECK(fake.sets == 0U);
    }
  }

  TEST_CASE("CoreAudioChannelLayout - required observations reject bounded storage violations before role admission")
  {
    auto fake = ChannelObservationFake{};

    SECTION("zero property bytes")
    {
      fake.reply(Property::ClientLayout).optBytes = std::vector<std::byte>{};
    }

    SECTION("property exceeds the 65536 byte bound")
    {
      fake.reply(Property::OutputLayout).optBytes = std::vector<std::byte>(65537U);
    }

    SECTION("zero output layout bytes")
    {
      fake.reply(Property::OutputLayout).optBytes = std::vector<std::byte>{};
    }

    SECTION("zero actual map bytes")
    {
      fake.reply(Property::OutputMap).optBytes = std::vector<std::byte>{};
    }

    SECTION("oversized actual client layout")
    {
      fake.reply(Property::ClientLayout).optBytes = std::vector<std::byte>(65537U);
    }

    SECTION("oversized actual map")
    {
      fake.reply(Property::OutputMap).optBytes = std::vector<std::byte>(65537U);
    }

    SECTION("zero descriptions")
    {
      fake.reply(Property::ClientLayout).optBytes = descriptions({});
    }

    SECTION("257 descriptions exceed the independent description bound")
    {
      auto bytes = std::vector<std::byte>(kHeaderBytes + (257U * sizeof(::AudioChannelDescription)));
      auto const header = std::to_array<::UInt32>({0U, 0U, 257U});
      std::memcpy(bytes.data(), header.data(), kHeaderBytes);
      fake.reply(Property::OutputLayout).optBytes = bytes;
    }

    SECTION("extra description storage is not an exact extent")
    {
      auto bytes = descriptions({1U, 2U});
      bytes.push_back(std::byte{0});
      fake.reply(Property::ClientLayout).optBytes = bytes;
    }

    SECTION("missing description storage is not an exact extent")
    {
      auto bytes = descriptions({1U, 2U});
      bytes.pop_back();
      fake.reply(Property::OutputLayout).optBytes = bytes;
    }

    SECTION("odd map byte count is not a destination array")
    {
      fake.reply(Property::OutputMap).optBytes = markers({0, 0, 0, 0, 1, 0, 0});
    }

    for (auto const speakerMask : {0U, 0x3U})
    {
      CAPTURE(speakerMask);
      auto client = kStereoClient;
      client.speakerMask = speakerMask;
      auto const planRes = configureCoreAudioChannels(fake.operations(), client, outputFormat());
      REQUIRE_FALSE(planRes);
      CHECK(planRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK(planRes.error().error.code == Error::Code::InitFailed);
      CHECK(fake.sets == 0U);
    }
  }

  TEST_CASE(
    "CoreAudioChannelLayout - both documented AU absence statuses apply to size and read without inventing evidence")
  {
    for (auto const property : {Property::ClientLayout, Property::OutputLayout, Property::OutputMap})
    {
      for (auto const status : {::kAudioUnitErr_InvalidProperty, ::kAudioUnitErr_PropertyNotInUse})
      {
        for (auto const duringRead : {false, true})
        {
          CAPTURE(property, status, duringRead);
          auto fake = ChannelObservationFake{};
          fake.reply(property).optBytes = property == Property::OutputMap ? ordinalMap({0, 1}) : descriptions({1U, 2U});

          if (duringRead)
          {
            fake.reply(property).readStatus = status;
          }
          else
          {
            fake.reply(property).sizeStatus = status;
          }

          auto const observedRes = observeCoreAudioChannels(fake.operations(), 2U, 2U);
          REQUIRE(observedRes);
          CHECK_FALSE(observedRes->optClientLabels);
          CHECK_FALSE(observedRes->optOutputLabels);
          CHECK_FALSE(observedRes->optOutputMap);
          CHECK(fake.reads == (duringRead ? 1U : 0U));
          auto const planRes = planCoreAudioChannels(kStereoClient, outputFormat(), *observedRes);
          REQUIRE(planRes);
          CHECK(planRes->sourceSlots == std::vector<std::uint32_t>{0U, 1U});
          CHECK_FALSE(planRes->delivery.isMapped);
          CHECK(planRes->delivery.nativeSpeakerMask == 0U);
          CHECK(fake.sets == 0U);
        }
      }
    }
  }
} // namespace ao::audio::backend::detail::test

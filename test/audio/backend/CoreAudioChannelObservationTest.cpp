// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioChannelLayout.h"
#include "lib/audio/backend/detail/CoreAudioDeviceDiscovery.h"
#include <ao/utility/ScopedRegistration.h>

#include <AudioToolbox/AUComponent.h>
#include <AudioToolbox/AudioComponent.h>
#include <AudioToolbox/AudioUnit.h>
#include <AudioToolbox/AudioUnitProperties.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <MacTypes.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <span>

namespace ao::audio::backend::test
{
  TEST_CASE("AUHAL - local output maps report identity and swapped native readback", "[.manual]")
  {
    // Explicit opt-in selects an existing exact device UID. No default route,
    // enumeration fallback, driver provisioning, or shared device setting changes.
    auto const* selectedUid = std::getenv("AOBUS_TEST_COREAUDIO_DEVICE_UID");
    REQUIRE(selectedUid != nullptr);
    REQUIRE(selectedUid[0] != '\0');
    WARN("Selected exact Core Audio device UID: " << selectedUid);
    auto const deviceRes = detail::coreAudioOutputDeviceId(selectedUid);

    if (!deviceRes)
    {
      WARN("Selected device resolution failed: " << deviceRes.error().message);
    }

    REQUIRE(deviceRes);
    auto const device = *deviceRes;
    WARN("Resolved Core Audio device object: " << device);

    auto componentDescription = ::AudioComponentDescription{};
    componentDescription.componentType = ::kAudioUnitType_Output;
    componentDescription.componentSubType = ::kAudioUnitSubType_HALOutput;
    componentDescription.componentManufacturer = ::kAudioUnitManufacturer_Apple;
    auto* const component = ::AudioComponentFindNext(nullptr, &componentDescription);
    REQUIRE(component != nullptr);

    ::AudioUnit unit = nullptr;
    bool initialized = false;
    ::OSStatus uninitializeStatus = ::noErr;
    ::OSStatus disposeStatus = ::noErr;
    auto unitCleanup = utility::ScopedRegistration{[&]
                                                   {
                                                     if (unit != nullptr)
                                                     {
                                                       if (initialized)
                                                       {
                                                         uninitializeStatus = ::AudioUnitUninitialize(unit);
                                                       }

                                                       disposeStatus = ::AudioComponentInstanceDispose(unit);
                                                     }
                                                   }};

    auto status = ::AudioComponentInstanceNew(component, &unit);
    WARN("Own AUHAL create OSStatus=" << status);
    REQUIRE(status == ::noErr);
    REQUIRE(unit != nullptr);

    ::UInt32 enabled = 1U;
    status = ::AudioUnitSetProperty(
      unit, ::kAudioOutputUnitProperty_EnableIO, ::kAudioUnitScope_Output, 0U, &enabled, sizeof(enabled));
    WARN("Own AUHAL enable output0 OSStatus=" << status);
    REQUIRE(status == ::noErr);
    ::UInt32 disabled = 0U;
    status = ::AudioUnitSetProperty(
      unit, ::kAudioOutputUnitProperty_EnableIO, ::kAudioUnitScope_Input, 1U, &disabled, sizeof(disabled));
    WARN("Own AUHAL disable input1 OSStatus=" << status);
    REQUIRE(status == ::noErr);
    status = ::AudioUnitSetProperty(
      unit, ::kAudioOutputUnitProperty_CurrentDevice, ::kAudioUnitScope_Global, 0U, &device, sizeof(device));
    WARN("Own AUHAL select exact device OSStatus=" << status);
    REQUIRE(status == ::noErr);

    auto client = ::AudioStreamBasicDescription{};
    client.mSampleRate = 48000.0;
    client.mFormatID = ::kAudioFormatLinearPCM;
    client.mFormatFlags = ::kAudioFormatFlagIsSignedInteger | ::kAudioFormatFlagIsPacked;
    client.mBytesPerPacket = 4U;
    client.mFramesPerPacket = 1U;
    client.mBytesPerFrame = 4U;
    client.mChannelsPerFrame = 2U;
    client.mBitsPerChannel = 16U;
    status = ::AudioUnitSetProperty(
      unit, ::kAudioUnitProperty_StreamFormat, ::kAudioUnitScope_Input, 0U, &client, sizeof(client));
    WARN("Own AUHAL request 48000Hz interleaved 2ch Signed16 client OSStatus=" << status);
    REQUIRE(status == ::noErr);

    // No callback is installed and the unit is never started or rendered.
    // Initialization and map changes belong only to this disposable output unit.
    status = ::AudioUnitInitialize(unit);
    initialized = status == ::noErr;
    WARN("Own AUHAL initialize without starting OSStatus=" << status);
    REQUIRE(status == ::noErr);

    auto appliedClient = ::AudioStreamBasicDescription{};
    auto clientSize = static_cast<::UInt32>(sizeof(appliedClient));
    status = ::AudioUnitGetProperty(
      unit, ::kAudioUnitProperty_StreamFormat, ::kAudioUnitScope_Input, 0U, &appliedClient, &clientSize);
    WARN("Own AUHAL client readback OSStatus=" << status << " bytes=" << clientSize);
    REQUIRE(status == ::noErr);
    REQUIRE(clientSize == sizeof(appliedClient));
    REQUIRE(appliedClient.mSampleRate == 48000.0);
    REQUIRE(appliedClient.mFormatID == ::kAudioFormatLinearPCM);
    REQUIRE(appliedClient.mFormatFlags == client.mFormatFlags);
    REQUIRE(appliedClient.mBytesPerPacket == 4U);
    REQUIRE(appliedClient.mFramesPerPacket == 1U);
    REQUIRE(appliedClient.mBytesPerFrame == 4U);
    REQUIRE(appliedClient.mChannelsPerFrame == 2U);
    REQUIRE(appliedClient.mBitsPerChannel == 16U);

    auto output = ::AudioStreamBasicDescription{};
    auto outputSize = static_cast<::UInt32>(sizeof(output));
    status = ::AudioUnitGetProperty(
      unit, ::kAudioUnitProperty_StreamFormat, ::kAudioUnitScope_Output, 0U, &output, &outputSize);
    WARN("Own AUHAL native output0 format OSStatus=" << status << " bytes=" << outputSize << " channels="
                                                     << output.mChannelsPerFrame << " rate=" << output.mSampleRate);
    REQUIRE(status == ::noErr);
    REQUIRE(outputSize == sizeof(output));
    REQUIRE(output.mChannelsPerFrame == 2U);

    // SDK ChannelMap entries are destination->source (-1 is silence). TN2091
    // playback uses input scope element0; the production operations below use
    // exactly that boundary, not the recording input-element1 direction.
    auto const operations = detail::coreAudioChannelOperations(unit, device);
    constexpr auto kRequests = std::array{std::to_array<::SInt32>({0, 1}), std::to_array<::SInt32>({1, 0})};
    std::size_t accepted = 0U;
    std::size_t refused = 0U;

    for (auto const& requested : kRequests)
    {
      auto const setStatus = operations.set(
        detail::CoreAudioChannelProperty::OutputMap, std::as_bytes(std::span<::SInt32 const>{requested}));
      WARN("AUHAL playback input0 ChannelMap request={" << requested[0] << ',' << requested[1]
                                                        << "} SET OSStatus=" << setStatus);

      if (setStatus != ::noErr)
      {
        // These are the production map-set refusal statuses, not a blanket
        // conversion of native device/resource/unknown errors into capability absence.
        REQUIRE((setStatus == ::kAudioUnitErr_InvalidProperty || setStatus == ::kAudioUnitErr_PropertyNotInUse ||
                 setStatus == ::kAudioUnitErr_FormatNotSupported));
        ++refused;
        WARN("CAPABILITY REFUSAL for request={" << requested[0] << ',' << requested[1] << "}, OSStatus=" << setStatus
                                                << "; no successful configuration/readback claimed");
        continue;
      }

      ::UInt32 mapSize = 0U;
      auto const sizeStatus = operations.size(detail::CoreAudioChannelProperty::OutputMap, mapSize);
      WARN("AUHAL ChannelMap request={" << requested[0] << ',' << requested[1] << "} size OSStatus=" << sizeStatus
                                        << " bytes=" << mapSize);
      // After successful SET, even a normally optional-property error is failure.
      REQUIRE(sizeStatus == ::noErr);
      REQUIRE(mapSize == sizeof(requested));
      auto readback = std::array<::SInt32, 2>{};
      auto readSize = static_cast<::UInt32>(sizeof(readback));
      auto const readStatus = operations.read(detail::CoreAudioChannelProperty::OutputMap, readSize, readback.data());
      WARN("AUHAL ChannelMap request={" << requested[0] << ',' << requested[1] << "} read OSStatus=" << readStatus
                                        << " bytes=" << readSize << " buffer={" << readback[0] << ',' << readback[1]
                                        << '}');
      REQUIRE(readStatus == ::noErr);
      REQUIRE(readSize == sizeof(readback));
      REQUIRE(readback == requested);
      ++accepted;
      WARN("ACCEPTANCE with exact-size native readback={"
           << readback[0] << ',' << readback[1]
           << "}; property API evidence only, no rendered/captured frames or physical routing proof");
    }

    WARN("Own AUHAL map control summary: accepted="
         << accepted << " refused=" << refused
         << "; refusal is not supported mapping, and acceptance is not capture/gain/all-device proof");
    CHECK(accepted + refused == 2U);
    unitCleanup.reset();
    WARN("Own AUHAL uninitialize OSStatus=" << uninitializeStatus << " dispose OSStatus=" << disposeStatus);
    CHECK(uninitializeStatus == ::noErr);
    CHECK(disposeStatus == ::noErr);
  }
} // namespace ao::audio::backend::test

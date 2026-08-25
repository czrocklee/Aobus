// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioDeviceDiscovery.h"

#include "CoreFoundationOwnership.h"
#include "CoreFoundationString.h"

#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail
{
  namespace
  {
    constexpr auto kGlobalMain = ::AudioObjectPropertyAddress{
      0U, ::kAudioObjectPropertyScopeGlobal, ::kAudioObjectPropertyElementMain};
    constexpr auto kOutputMain = ::AudioObjectPropertyAddress{
      0U, ::kAudioObjectPropertyScopeOutput, ::kAudioObjectPropertyElementMain};

    template<typename Value>
    bool readScalar(::AudioObjectID const object,
                    ::AudioObjectPropertyAddress address,
                    Value& value) noexcept
    {
      auto size = static_cast<::UInt32>(sizeof(Value));
      return ::AudioObjectGetPropertyData(object, &address, 0U, nullptr, &size, &value) == ::noErr &&
             size == sizeof(Value);
    }

    std::vector<::AudioObjectID> readObjectList(::AudioObjectID const object,
                                                ::AudioObjectPropertyAddress address)
    {
      ::UInt32 byteCount = 0U;
      if (::AudioObjectGetPropertyDataSize(object, &address, 0U, nullptr, &byteCount) != ::noErr ||
          byteCount % sizeof(::AudioObjectID) != 0U)
      {
        return {};
      }

      auto values = std::vector<::AudioObjectID>(byteCount / sizeof(::AudioObjectID));
      if (byteCount != 0U &&
          ::AudioObjectGetPropertyData(object, &address, 0U, nullptr, &byteCount, values.data()) != ::noErr)
      {
        return {};
      }
      values.resize(byteCount / sizeof(::AudioObjectID));
      return values;
    }

    std::string readString(::AudioObjectID const object,
                           ::AudioObjectPropertySelector const selector)
    {
      auto address = kGlobalMain;
      address.mSelector = selector;
      ::CFStringRef rawValue = nullptr;
      if (!readScalar(object, address, rawValue) || rawValue == nullptr)
      {
        return {};
      }

      auto valuePtr = CoreFoundationPtr<::CFStringRef>{rawValue};
      auto const utf8Res = utf8String(valuePtr.get());
      return utf8Res ? *utf8Res : std::string{};
    }

    bool isOutputDevice(::AudioDeviceID const device)
    {
      auto aliveAddress = kGlobalMain;
      aliveAddress.mSelector = ::kAudioDevicePropertyDeviceIsAlive;
      ::UInt32 alive = 0U;
      if (!readScalar(device, aliveAddress, alive) || alive == 0U)
      {
        return false;
      }

      auto configurationAddress = kOutputMain;
      configurationAddress.mSelector = ::kAudioDevicePropertyStreamConfiguration;
      ::UInt32 byteCount = 0U;
      if (::AudioObjectGetPropertyDataSize(
            device, &configurationAddress, 0U, nullptr, &byteCount) != ::noErr ||
          byteCount < offsetof(::AudioBufferList, mBuffers))
      {
        return false;
      }

      auto storage = std::vector<std::byte>(byteCount);
      if (::AudioObjectGetPropertyData(
            device, &configurationAddress, 0U, nullptr, &byteCount, storage.data()) != ::noErr ||
          byteCount < offsetof(::AudioBufferList, mBuffers))
      {
        return false;
      }

      auto const* buffers = reinterpret_cast<::AudioBufferList const*>(storage.data());
      auto const availableBufferCount =
        (byteCount - offsetof(::AudioBufferList, mBuffers)) / sizeof(::AudioBuffer);
      if (buffers->mNumberBuffers > availableBufferCount)
      {
        return false;
      }

      for (::UInt32 index = 0U; index < buffers->mNumberBuffers; ++index)
      {
        if (buffers->mBuffers[index].mNumberChannels != 0U)
        {
          return true;
        }
      }
      return false;
    }

    std::vector<::AudioDeviceID> audioDeviceIds()
    {
      auto address = kGlobalMain;
      address.mSelector = ::kAudioHardwarePropertyDevices;
      auto objects = readObjectList(::kAudioObjectSystemObject, address);
      return {objects.begin(), objects.end()};
    }

    ::AudioDeviceID defaultOutputDevice()
    {
      auto address = kGlobalMain;
      address.mSelector = ::kAudioHardwarePropertyDefaultOutputDevice;
      auto result = ::AudioDeviceID{kAudioObjectUnknown};
      readScalar(::kAudioObjectSystemObject, address, result);
      return result;
    }
  } // namespace

  std::vector<Device> orderCoreAudioDevices(std::vector<Device> devices, std::string_view const defaultDeviceUid)
  {
    for (auto& device : devices)
    {
      device.isDefault = device.id == defaultDeviceUid;
    }

    std::ranges::sort(devices,
                      [](Device const& lhs, Device const& rhs)
                      {
                        return std::tuple{!lhs.isDefault, lhs.displayName, lhs.id.raw()} <
                               std::tuple{!rhs.isDefault, rhs.displayName, rhs.id.raw()};
                      });
    return devices;
  }

  std::vector<Device> enumerateCoreAudioOutputDevices()
  {
    auto devices = std::vector<Device>{};
    auto const defaultDevice = defaultOutputDevice();
    auto defaultUid = std::string{};

    for (auto const deviceId : audioDeviceIds())
    {
      if (!isOutputDevice(deviceId))
      {
        continue;
      }

      auto uid = readString(deviceId, ::kAudioDevicePropertyDeviceUID);
      if (uid.empty())
      {
        continue;
      }

      if (deviceId == defaultDevice)
      {
        defaultUid = uid;
      }

      auto name = readString(deviceId, ::kAudioObjectPropertyName);
      if (name.empty())
      {
        name = uid;
      }

      devices.push_back({.id = DeviceId{std::move(uid)},
                         .displayName = std::move(name),
                         .description = readString(deviceId, ::kAudioObjectPropertyManufacturer),
                         .backendId = kBackendCoreAudio});
    }

    return orderCoreAudioDevices(std::move(devices), defaultUid);
  }

  Result<::AudioDeviceID> coreAudioOutputDeviceId(std::string_view const deviceUid)
  {
    auto const uidRes = coreFoundationString(deviceUid);
    if (!uidRes)
    {
      return std::unexpected(uidRes.error());
    }

    auto const uid = uidRes->get();
    auto deviceId = ::AudioDeviceID{::kAudioObjectUnknown};
    auto byteCount = static_cast<::UInt32>(sizeof(deviceId));
    auto address = kGlobalMain;
    address.mSelector = ::kAudioHardwarePropertyTranslateUIDToDevice;
    auto const status = ::AudioObjectGetPropertyData(::kAudioObjectSystemObject,
                                                     &address,
                                                     sizeof(uid),
                                                     &uid,
                                                     &byteCount,
                                                     &deviceId);
    if (status == ::noErr && byteCount == sizeof(deviceId) && deviceId != ::kAudioObjectUnknown &&
        isOutputDevice(deviceId))
    {
      return deviceId;
    }

    return makeError(Error::Code::DeviceNotFound, "Core Audio output device is no longer available");
  }
} // namespace ao::audio::backend::detail

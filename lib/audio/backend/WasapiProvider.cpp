// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/backend/WasapiProvider.h>
#include <ao/audio/backend/WasapiSharedBackend.h>
#include <ao/audio/backend/detail/AudioBackendFormatSupport.h>
#include <ao/audio/backend/detail/WasapiGraphRegistry.h>
#include <ao/audio/backend/detail/WasapiProviderMonitorHooks.h>
#include <ao/audio/backend/detail/WasapiStrings.h>
#include <ao/utility/ThreadName.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <initguid.h>
#include <mmdeviceapi.h>
#include <windows.h>

// Requires the PROPERTYKEY machinery mmdeviceapi.h pulls in; keep it after.
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

// winspool.h (dragged in via windows.h) defines DeviceCapabilities as a macro,
// clobbering ao::audio::DeviceCapabilities.
#ifdef DeviceCapabilities
#undef DeviceCapabilities
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::backend
{
  namespace
  {
    using Microsoft::WRL::ComPtr;

    // Plug/unplug bursts fire several endpoint notifications back to back;
    // wait briefly so one re-enumeration covers the whole burst.
    constexpr auto kChangeCoalesceDelay = std::chrono::milliseconds{250};
    constexpr auto kCallbackGatePollDelay = std::chrono::milliseconds{10};

    /**
     * @brief Keeps the process-wide COM multithreaded apartment alive.
     *
     * Threads that never call CoInitializeEx become implicit members of the
     * MTA while this usage is held, so provider threads can use the MMDevice
     * interfaces without per-thread apartment management.
     */
    class MtaUsage final
    {
    public:
      MtaUsage() noexcept
        : _active{SUCCEEDED(::CoIncrementMTAUsage(&_cookie))}
      {
      }

      ~MtaUsage()
      {
        if (_active)
        {
          ::CoDecrementMTAUsage(_cookie);
        }
      }

      MtaUsage(MtaUsage const&) = delete;
      MtaUsage& operator=(MtaUsage const&) = delete;
      MtaUsage(MtaUsage&&) = delete;
      MtaUsage& operator=(MtaUsage&&) = delete;

    private:
      CO_MTA_USAGE_COOKIE _cookie{};
      bool _active = false;
    };

    DeviceCapabilities sharedModeCapabilities()
    {
      auto caps = DeviceCapabilities{};

      // Shared mode with AUTOCONVERTPCM accepts any sample rate and channel
      // count (the audio engine converts), so those lists stay empty, which
      // FormatNegotiator treats as unconstrained.
      detail::addSampleFormatCapability(caps, {.bitDepth = 16, .validBits = 16, .isFloat = false});
      detail::addSampleFormatCapability(caps, {.bitDepth = 24, .validBits = 24, .isFloat = false});
      detail::addSampleFormatCapability(caps, {.bitDepth = 32, .validBits = 24, .isFloat = false});
      detail::addSampleFormatCapability(caps, {.bitDepth = 32, .validBits = 32, .isFloat = false});
      detail::addSampleFormatCapability(caps, {.bitDepth = 32, .validBits = 32, .isFloat = true});

      return caps;
    }

    std::string friendlyName(IMMDevice* device, std::string const& fallback)
    {
      auto store = ComPtr<IPropertyStore>{};

      if (FAILED(device->OpenPropertyStore(STGM_READ, &store)))
      {
        return fallback;
      }

      auto value = PROPVARIANT{};
      ::PropVariantInit(&value);

      auto name = fallback;

      // PROPVARIANT is a Windows C union; vt is checked before reading pwszVal.
      // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
      if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR &&
          value.pwszVal != nullptr)
      {
        name = detail::wideToUtf8(value.pwszVal);
      }
      // NOLINTEND(cppcoreguidelines-pro-type-union-access)

      ::PropVariantClear(&value);
      return name;
    }

    std::wstring defaultEndpointId(IMMDeviceEnumerator* enumerator)
    {
      auto device = ComPtr<IMMDevice>{};

      if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)))
      {
        return {};
      }

      LPWSTR rawId = nullptr;

      if (FAILED(device->GetId(&rawId)) || rawId == nullptr)
      {
        return {};
      }

      auto id = std::wstring{rawId};
      ::CoTaskMemFree(rawId);
      return id;
    }

    std::vector<Device> enumerateWasapiRenderDevices(IMMDeviceEnumerator* enumerator)
    {
      auto devices = std::vector<Device>{};

      auto collection = ComPtr<IMMDeviceCollection>{};

      if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)))
      {
        return devices;
      }

      UINT count = 0;

      if (FAILED(collection->GetCount(&count)))
      {
        return devices;
      }

      auto const defaultId = defaultEndpointId(enumerator);

      for (UINT index = 0; index < count; ++index)
      {
        auto device = ComPtr<IMMDevice>{};

        if (FAILED(collection->Item(index, &device)))
        {
          continue;
        }

        LPWSTR rawId = nullptr;

        if (FAILED(device->GetId(&rawId)) || rawId == nullptr)
        {
          continue;
        }

        auto const wideId = std::wstring{rawId};
        ::CoTaskMemFree(rawId);

        auto utf8Id = detail::wideToUtf8(wideId);
        auto name = friendlyName(device.Get(), utf8Id);

        devices.push_back({.id = DeviceId{std::move(utf8Id)},
                           .displayName = std::move(name),
                           .description = "WASAPI render endpoint",
                           .isDefault = (wideId == defaultId),
                           .backendId = kBackendWasapi,
                           .capabilities = sharedModeCapabilities()});
      }

      // PlaybackService auto-selects the first device of the first backend, so
      // surface the system default endpoint first.
      std::ranges::stable_partition(devices, std::identity{}, &Device::isDefault);

      return devices;
    }
  } // namespace

  struct WasapiProvider::Impl final
  {
    /**
     * @brief Endpoint notification sink registered with the device enumerator.
     *
     * Lifetime is owned by Impl (member, no heap refcount): the enumerator is
     * always unregistered before destruction, so AddRef/Release are inert as
     * in the documented member-embedded IMMNotificationClient pattern.
     */
    class NotificationClient final : public IMMNotificationClient
    {
    public:
      explicit NotificationClient(std::function<void()> onChanged)
        : _onChanged{std::move(onChanged)}
      {
      }

      ~NotificationClient() = default;

      NotificationClient(NotificationClient const&) = delete;
      NotificationClient& operator=(NotificationClient const&) = delete;
      NotificationClient(NotificationClient&&) = delete;
      NotificationClient& operator=(NotificationClient&&) = delete;

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
      {
        if (object == nullptr)
        {
          return E_POINTER;
        }

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
        {
          *object = static_cast<IMMNotificationClient*>(this);
          return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
      }

      ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
      ULONG STDMETHODCALLTYPE Release() override { return 1; }

      HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR /*deviceId*/, DWORD /*newState*/) override
      {
        _onChanged();
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*deviceId*/) override
      {
        _onChanged();
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*deviceId*/) override
      {
        _onChanged();
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole /*role*/, LPCWSTR /*deviceId*/) override
      {
        if (flow == eRender)
        {
          _onChanged();
        }

        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*deviceId*/, PROPERTYKEY /*key*/) override
      {
        // Friendly-name changes arrive through this callback. Re-enumeration is
        // coalesced by the monitor thread, so treating all endpoint property
        // changes alike keeps display names current without callback-side COM.
        _onChanged();
        return S_OK;
      }

    private:
      std::function<void()> _onChanged;
    };

    struct DeviceSub
    {
      std::uint64_t id;
      OnDevicesChangedCallback callback;
    };

    struct MonitorState final
    {
      MtaUsage mtaUsage;
      std::shared_ptr<detail::WasapiProviderMonitorHooks> monitorHooksPtr;

      mutable std::mutex mutex;
      mutable std::recursive_timed_mutex callbackMutex;
      std::vector<Device> cachedDevices;

      ComPtr<IMMDeviceEnumerator> enumerator;
      NotificationClient notificationClient;
      bool notificationsRegistered = false;

      HANDLE stopEvent = nullptr;
      HANDLE changeEvent = nullptr;

      std::vector<DeviceSub> deviceSubs;
      std::uint64_t nextSubId = 1;
      std::atomic<bool> shutdownRequested{false};

      explicit MonitorState(std::shared_ptr<detail::WasapiProviderMonitorHooks> hooksPtr)
        // The notification callback only signals an event: re-enumerating inside
        // an IMMNotificationClient callback risks deadlocking the MMDevice lock.
        : monitorHooksPtr{std::move(hooksPtr)}
        , notificationClient{[this]
                             {
                               if (changeEvent != nullptr)
                               {
                                 ::SetEvent(changeEvent);
                               }
                             }}
        , stopEvent{::CreateEventW(nullptr, TRUE, FALSE, nullptr)}
        , changeEvent{::CreateEventW(nullptr, FALSE, FALSE, nullptr)}
      {
        if (monitorHooksPtr && monitorHooksPtr->enumerateDevices)
        {
          cachedDevices = monitorHooksPtr->enumerateDevices();
        }
        else if (SUCCEEDED(
                   ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        {
          cachedDevices = enumerateWasapiRenderDevices(enumerator.Get());
          notificationsRegistered = SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(&notificationClient));
        }
      }

      ~MonitorState()
      {
        requestShutdown();

        if (changeEvent != nullptr)
        {
          ::CloseHandle(changeEvent);
        }

        if (stopEvent != nullptr)
        {
          ::CloseHandle(stopEvent);
        }

        if (monitorHooksPtr && monitorHooksPtr->onMonitorStateDestroyed)
        {
          try
          {
            monitorHooksPtr->onMonitorStateDestroyed();
          }
          // NOLINTNEXTLINE(bugprone-empty-catch): Lifecycle observers cannot unwind a destructor.
          catch (...)
          {
            // Lifecycle observers cannot unwind a destructor path.
          }
        }
      }

      MonitorState(MonitorState const&) = delete;
      MonitorState& operator=(MonitorState const&) = delete;
      MonitorState(MonitorState&&) = delete;
      MonitorState& operator=(MonitorState&&) = delete;

      std::vector<Device> enumerateDevices() const
      {
        if (monitorHooksPtr && monitorHooksPtr->enumerateDevices)
        {
          return monitorHooksPtr->enumerateDevices();
        }

        return enumerator.Get() != nullptr ? enumerateWasapiRenderDevices(enumerator.Get()) : std::vector<Device>{};
      }

      bool acquireCallbackGate(std::unique_lock<std::recursive_timed_mutex>& callbackLock) const
      {
        while (!shutdownRequested.load(std::memory_order_acquire))
        {
          if (callbackLock.try_lock_for(kCallbackGatePollDelay))
          {
            return true;
          }
        }

        return false;
      }

      bool deviceSubscriptionIsActive(std::uint64_t id) const
      {
        auto const lock = std::scoped_lock{mutex};
        return !shutdownRequested.load(std::memory_order_relaxed) &&
               std::ranges::find(deviceSubs, id, &DeviceSub::id) != deviceSubs.end();
      }

      void removeDeviceSubscription(std::uint64_t id)
      {
        auto const lock = std::scoped_lock{mutex};
        auto const it = std::ranges::find(deviceSubs, id, &DeviceSub::id);

        if (it != deviceSubs.end())
        {
          deviceSubs.erase(it);
        }
      }

      void notifyDeviceCallbacksReady() const noexcept
      {
        if (!monitorHooksPtr || !monitorHooksPtr->onDeviceCallbacksReady)
        {
          return;
        }

        try
        {
          monitorHooksPtr->onDeviceCallbacksReady();
        }
        catch (...)
        {
          // Test and embedding observers cannot unwind the monitor thread.
          return;
        }
      }

      bool deliverDeviceCallback(DeviceSub const& sub, std::vector<Device> const& snapshot)
      {
        auto callbackLock = std::unique_lock{callbackMutex, std::defer_lock};

        if (!acquireCallbackGate(callbackLock))
        {
          return false;
        }

        if (!deviceSubscriptionIsActive(sub.id))
        {
          return true;
        }

        try
        {
          if (sub.callback)
          {
            sub.callback(snapshot);
          }
        }
        catch (...)
        {
          // Initial delivery removes a callback before rethrowing to its
          // caller. Match that policy here without unwinding the worker.
          removeDeviceSubscription(sub.id);
        }

        return true;
      }

      bool notifyRefreshComplete()
      {
        if (!monitorHooksPtr || !monitorHooksPtr->onRefreshComplete)
        {
          return true;
        }

        auto callbackLock = std::unique_lock{callbackMutex, std::defer_lock};

        if (!acquireCallbackGate(callbackLock) || shutdownRequested.load(std::memory_order_relaxed))
        {
          return false;
        }

        try
        {
          monitorHooksPtr->onRefreshComplete();
        }
        // NOLINTNEXTLINE(bugprone-empty-catch): Monitor observers cannot unwind the worker thread.
        catch (...)
        {
          // Lifecycle observers cannot unwind the monitor thread.
        }

        return true;
      }

      void monitorLoop()
      {
        auto const handles = std::array<HANDLE, 2>{stopEvent, changeEvent};

        while (!shutdownRequested.load(std::memory_order_acquire))
        {
          auto const waited =
            ::WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);

          if (waited != WAIT_OBJECT_0 + 1)
          {
            return; // stop requested or wait failure
          }

          // Coalesce bursts while remaining immediately interruptible by shutdown.
          if (::WaitForSingleObject(stopEvent, static_cast<DWORD>(kChangeCoalesceDelay.count())) == WAIT_OBJECT_0)
          {
            return;
          }

          ::ResetEvent(changeEvent); // absorb notifications that arrived while coalescing

          auto newDevices = enumerateDevices();
          auto subs = std::vector<DeviceSub>{};
          auto snapshot = std::vector<Device>{};

          {
            auto const lock = std::scoped_lock{mutex};

            if (shutdownRequested.load(std::memory_order_relaxed))
            {
              return;
            }

            cachedDevices = std::move(newDevices);
            snapshot = cachedDevices;
            subs = deviceSubs;
          }

          notifyDeviceCallbacksReady();

          for (auto const& sub : subs)
          {
            if (!deliverDeviceCallback(sub, snapshot))
            {
              return;
            }
          }

          if (!notifyRefreshComplete())
          {
            return;
          }
        }
      }

      // Lock failure at this noexcept lifecycle boundary is unrecoverable and
      // intentionally retains the provider's fail-fast contract.
      // NOLINTNEXTLINE(bugprone-exception-escape)
      void requestShutdown() noexcept
      {
        if (shutdownRequested.exchange(true, std::memory_order_acq_rel))
        {
          return;
        }

        if (stopEvent != nullptr)
        {
          ::SetEvent(stopEvent);
        }

        if (notificationsRegistered && enumerator.Get() != nullptr)
        {
          enumerator->UnregisterEndpointNotificationCallback(&notificationClient);
          notificationsRegistered = false;
        }

        auto const callbackLock = std::scoped_lock{callbackMutex};
        auto const lock = std::scoped_lock{mutex};
        cachedDevices.clear();
        deviceSubs.clear();
      }
    };

    std::shared_ptr<detail::WasapiGraphRegistry> graphRegistryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    std::shared_ptr<MonitorState> monitorStatePtr;
    std::jthread monitorThread;
    std::atomic<bool> shutdownStarted{false};

    explicit Impl(std::shared_ptr<detail::WasapiProviderMonitorHooks> monitorHooksPtr)
      : monitorStatePtr{std::make_shared<MonitorState>(std::move(monitorHooksPtr))}
    {
      if (monitorStatePtr->monitorHooksPtr)
      {
        monitorStatePtr->monitorHooksPtr->requestRefresh = [weakStatePtr = std::weak_ptr{monitorStatePtr}]
        {
          if (auto const statePtr = weakStatePtr.lock(); statePtr && statePtr->changeEvent != nullptr)
          {
            ::SetEvent(statePtr->changeEvent);
          }
        };
      }

      if ((monitorStatePtr->enumerator.Get() != nullptr ||
           (monitorStatePtr->monitorHooksPtr && monitorStatePtr->monitorHooksPtr->enumerateDevices)) &&
          monitorStatePtr->stopEvent != nullptr && monitorStatePtr->changeEvent != nullptr)
      {
        monitorThread = std::jthread{[statePtr = monitorStatePtr]
                                     {
                                       setCurrentThreadName("WasapiDeviceMonitor");
                                       statePtr->monitorLoop();

                                       if (statePtr->monitorHooksPtr && statePtr->monitorHooksPtr->onMonitorExit)
                                       {
                                         try
                                         {
                                           statePtr->monitorHooksPtr->onMonitorExit();
                                         }
                                         // NOLINTNEXTLINE(bugprone-empty-catch): Exit observers cannot unwind a thread.
                                         catch (...)
                                         {
                                           // Lifecycle observers cannot unwind the monitor thread.
                                         }
                                       }
                                     }};
      }
    }

    ~Impl() { shutdown(); }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void shutdown() noexcept
    {
      if (shutdownStarted.exchange(true, std::memory_order_acq_rel))
      {
        return;
      }

      auto const graphPtr = graphRegistryPtr;
      monitorStatePtr->requestShutdown();

      if (monitorThread.joinable())
      {
        if (std::this_thread::get_id() == monitorThread.get_id())
        {
          // The callback may own and destroy the provider. The thread keeps
          // MonitorState alive through its shared capture and exits after the
          // callback returns, so detaching here avoids jthread self-join safely.
          monitorThread.detach();
        }
        else
        {
          monitorThread.join();
        }
      }

      // Keep this as the final operation: a graph callback may destroy the
      // provider, while the local shared owner keeps the registry call safe.
      graphPtr->shutdown();
    }
  };

  WasapiProvider::WasapiProvider()
    : WasapiProvider{nullptr}
  {
  }

  WasapiProvider::WasapiProvider(std::shared_ptr<detail::WasapiProviderMonitorHooks> monitorHooksPtr)
    : _implPtr{std::make_unique<Impl>(std::move(monitorHooksPtr))}
  {
  }

  WasapiProvider::~WasapiProvider()
  {
    shutdown();
  }

  void WasapiProvider::shutdown() noexcept
  {
    _implPtr->shutdown();
  }

  Subscription WasapiProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    if (!callback)
    {
      return {};
    }

    auto const statePtr = _implPtr->monitorStatePtr;
    std::uint64_t id = 0;
    auto devices = std::vector<Device>{};
    // Linearize registration, snapshot capture, and initial delivery with
    // monitor refresh callbacks without holding the device-state lock in user code.
    auto const callbackLock = std::scoped_lock{statePtr->callbackMutex};

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdownRequested.load(std::memory_order_relaxed))
      {
        return {};
      }

      id = statePtr->nextSubId++;
      statePtr->deviceSubs.push_back({.id = id, .callback = callback});
      devices = statePtr->cachedDevices;
    }

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdownRequested.load(std::memory_order_relaxed) ||
          std::ranges::find(statePtr->deviceSubs, id, &Impl::DeviceSub::id) == statePtr->deviceSubs.end())
      {
        return {};
      }
    }

    try
    {
      callback(devices);
    }
    catch (...)
    {
      auto const lock = std::scoped_lock{statePtr->mutex};
      auto const it = std::ranges::find(statePtr->deviceSubs, id, &Impl::DeviceSub::id);

      if (it != statePtr->deviceSubs.end())
      {
        statePtr->deviceSubs.erase(it);
      }

      throw;
    }

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->shutdownRequested.load(std::memory_order_relaxed) ||
          std::ranges::find(statePtr->deviceSubs, id, &Impl::DeviceSub::id) == statePtr->deviceSubs.end())
      {
        return {};
      }
    }

    return Subscription{[weakStatePtr = std::weak_ptr{statePtr}, id]
                        {
                          auto const statePtr = weakStatePtr.lock();

                          if (!statePtr)
                          {
                            return;
                          }

                          auto const callbackLock = std::scoped_lock{statePtr->callbackMutex};
                          auto const lock = std::scoped_lock{statePtr->mutex};
                          auto const it = std::ranges::find(statePtr->deviceSubs, id, &Impl::DeviceSub::id);

                          if (it != statePtr->deviceSubs.end())
                          {
                            statePtr->deviceSubs.erase(it);
                          }
                        }};
  }

  BackendProvider::Status WasapiProvider::status() const
  {
    auto const statePtr = _implPtr->monitorStatePtr;
    auto const lock = std::scoped_lock{statePtr->mutex};
    return {.descriptor = {.id = kBackendWasapi,
                           .name = "WASAPI",
                           .description = "Windows Audio Session API",
                           .iconName = "audio-card-symbolic",
                           .supportedProfiles = {{.id = kProfileShared,
                                                  .name = "Shared Mode",
                                                  .description = "System-level mixing with other applications"}}},
            .devices = statePtr->cachedDevices};
  }

  std::unique_ptr<Backend> WasapiProvider::createBackend(Device const& device, ProfileId const& /*profile*/)
  {
    return std::make_unique<WasapiSharedBackend>(device, kProfileShared, _implPtr->graphRegistryPtr);
  }

  Subscription WasapiProvider::subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback)
  {
    return _implPtr->graphRegistryPtr->subscribe(routeAnchor, std::move(callback));
  }
} // namespace ao::audio::backend

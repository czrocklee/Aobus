// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "backend/WasapiProvider.h"

#include "backend/WasapiSharedBackend.h"
#include "backend/detail/WasapiGraphRegistry.h"
#include "backend/detail/WasapiProviderMonitorHooks.h"
#include "backend/detail/WasapiStrings.h"
#include <ao/Contract.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/CallbackStackScope.h>
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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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

    void invokeHook(std::function<void()> const& hook, char const* const context) noexcept
    {
      if (!hook)
      {
        return;
      }

      try
      {
        hook();
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), context);
      }
    }

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
                           .isDefault = (wideId == defaultId),
                           .backendId = kBackendWasapi});
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
          AddRef();
          return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
      }

      ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
      ULONG STDMETHODCALLTYPE Release() override { return 1; }

      HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR /*deviceId*/, DWORD /*newState*/) override
      {
        return notifyChanged();
      }

      HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*deviceId*/) override { return notifyChanged(); }

      HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*deviceId*/) override { return notifyChanged(); }

      HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole /*role*/, LPCWSTR /*deviceId*/) override
      {
        if (flow == eRender)
        {
          return notifyChanged();
        }

        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*deviceId*/, PROPERTYKEY /*key*/) override
      {
        // Friendly-name changes arrive through this callback. Re-enumeration is
        // coalesced by the monitor thread, so treating all endpoint property
        // changes alike keeps display names current without callback-side COM.
        return notifyChanged();
      }

    private:
      HRESULT notifyChanged() noexcept
      {
        try
        {
          _onChanged();
          return S_OK;
        }
        catch (...)
        {
          AO_FATAL_EXCEPTION(std::current_exception(), "WASAPI endpoint-notification callback");
        }
      }

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
          catch (...)
          {
            AO_FATAL_EXCEPTION(std::current_exception(), "WASAPI monitor destruction callback");
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
          AO_FATAL_EXCEPTION(std::current_exception(), "WASAPI device-callback readiness observer");
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
          removeDeviceSubscription(sub.id);
          AO_FATAL_EXCEPTION(std::current_exception(), "WASAPI device observer");
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
        catch (...)
        {
          AO_FATAL_EXCEPTION(std::current_exception(), "WASAPI refresh-completion observer");
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

    class Control final : public std::enable_shared_from_this<Control>
    {
    public:
      explicit Control(std::shared_ptr<detail::WasapiProviderMonitorHooks> monitorHooksPtr)
        : _graphRegistryPtr{std::make_shared<detail::WasapiGraphRegistry>()}
        , _monitorStatePtr{std::make_shared<MonitorState>(std::move(monitorHooksPtr))}
      {
      }

      ~Control() { shutdown(); }

      Control(Control const&) = delete;
      Control& operator=(Control const&) = delete;
      Control(Control&&) = delete;
      Control& operator=(Control&&) = delete;

      void startMonitor()
      {
        auto const statePtr = _monitorStatePtr;

        if (statePtr->monitorHooksPtr)
        {
          statePtr->monitorHooksPtr->requestRefresh = [weakStatePtr = std::weak_ptr{statePtr}]
          {
            if (auto const retainedStatePtr = weakStatePtr.lock();
                retainedStatePtr && retainedStatePtr->changeEvent != nullptr)
            {
              ::SetEvent(retainedStatePtr->changeEvent);
            }
          };
        }

        if ((statePtr->enumerator.Get() == nullptr &&
             (!statePtr->monitorHooksPtr || !statePtr->monitorHooksPtr->enumerateDevices)) ||
            statePtr->stopEvent == nullptr || statePtr->changeEvent == nullptr)
        {
          markMonitorExited();
          return;
        }

        try
        {
          auto const retainedControlPtr = shared_from_this();
          _monitorThread =
            std::jthread{[retainedControlPtr, statePtr]
                         {
                           retainedControlPtr->markMonitorStarted();

                           try
                           {
                             setCurrentThreadName("WasapiDeviceMonitor");
                             statePtr->monitorLoop();

                             if (statePtr->monitorHooksPtr)
                             {
                               invokeHook(statePtr->monitorHooksPtr->onMonitorExit, "WASAPI monitor-exit observer");
                             }
                           }
                           catch (...)
                           {
                             AO_FATAL_EXCEPTION(std::current_exception(), "WASAPI device-monitor thread");
                           }

                           retainedControlPtr->markMonitorExited();
                         }};
        }
        catch (...)
        {
          markMonitorExited();
          throw;
        }
      }

      void shutdown() noexcept
      {
        bool returnWithoutWaiting = false;
        bool startedShutdown = false;
        bool waitForExistingShutdown = false;

        {
          auto const lock = std::scoped_lock{_lifecycleMutex};
          returnWithoutWaiting = isCurrentCallback() || _monitorThreadId == std::this_thread::get_id();

          if (_lifecycle == Lifecycle::Stopped)
          {
            return;
          }

          if (_lifecycle == Lifecycle::Stopping)
          {
            if (returnWithoutWaiting)
            {
              return;
            }

            waitForExistingShutdown = true;
          }
          else
          {
            _lifecycle = Lifecycle::Stopping;
            startedShutdown = true;
          }
        }

        if (waitForExistingShutdown)
        {
          notifyShutdownWait();
          waitForCompletion();
          return;
        }

        if (startedShutdown)
        {
          notifyShutdownStarted();
        }

        // Retain the COM-bearing monitor state until endpoint notifications and
        // both callback registries have retired. Callback-origin shutdown then
        // leaves these shared owners alive until callback and worker unwind.
        auto const monitorStatePtr = _monitorStatePtr;
        auto const graphRegistryPtr = _graphRegistryPtr;
        monitorStatePtr->requestShutdown();
        graphRegistryPtr->shutdown();

        if (_monitorThread.joinable())
        {
          if (returnWithoutWaiting)
          {
            _monitorThread.detach();
          }
          else
          {
            _monitorThread.join();
          }
        }

        {
          auto lock = std::unique_lock{_lifecycleMutex};
          _registriesRetired = true;
          updateCompletionLocked();

          if (returnWithoutWaiting)
          {
            return;
          }

          _completionChanged.wait(lock, [this] { return _lifecycle == Lifecycle::Stopped; });
        }
      }

      Subscription subscribeDevices(OnDevicesChangedCallback callback)
      {
        if (!callback || !acceptsSubscriptions())
        {
          return {};
        }

        auto const weakControlPtr = weak_from_this();
        callback = [weakControlPtr, callback = std::move(callback)](std::vector<Device> const& devices)
        {
          if (auto const controlPtr = weakControlPtr.lock(); controlPtr)
          {
            controlPtr->invokeDeviceCallback(callback, devices);
          }
        };

        auto const statePtr = _monitorStatePtr;
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
              std::ranges::find(statePtr->deviceSubs, id, &DeviceSub::id) == statePtr->deviceSubs.end())
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
          auto const it = std::ranges::find(statePtr->deviceSubs, id, &DeviceSub::id);

          if (it != statePtr->deviceSubs.end())
          {
            statePtr->deviceSubs.erase(it);
          }

          AO_FATAL_EXCEPTION(std::current_exception(), "WASAPI device observer");
        }

        {
          auto const lock = std::scoped_lock{statePtr->mutex};

          if (statePtr->shutdownRequested.load(std::memory_order_relaxed) ||
              std::ranges::find(statePtr->deviceSubs, id, &DeviceSub::id) == statePtr->deviceSubs.end())
          {
            return {};
          }
        }

        if (!acceptsSubscriptions())
        {
          statePtr->removeDeviceSubscription(id);
          return {};
        }

        return Subscription{[weakStatePtr = std::weak_ptr{statePtr}, id]
                            {
                              auto const retainedStatePtr = weakStatePtr.lock();

                              if (!retainedStatePtr)
                              {
                                return;
                              }

                              auto const callbackLock = std::scoped_lock{retainedStatePtr->callbackMutex};
                              retainedStatePtr->removeDeviceSubscription(id);
                            }};
      }

      Subscription subscribeGraph(std::string_view const routeAnchor, OnGraphChangedCallback callback)
      {
        if (!callback || !acceptsSubscriptions())
        {
          return {};
        }

        auto const weakControlPtr = weak_from_this();
        auto sub =
          _graphRegistryPtr->subscribe(routeAnchor,
                                       [weakControlPtr, callback = std::move(callback)](flow::Graph const& graph)
                                       {
                                         if (auto const controlPtr = weakControlPtr.lock(); controlPtr)
                                         {
                                           controlPtr->invokeGraphCallback(callback, graph);
                                         }
                                       });

        if (!acceptsSubscriptions())
        {
          sub.reset();
          return {};
        }

        return sub;
      }

      std::vector<Device> devices() const
      {
        auto const statePtr = _monitorStatePtr;
        auto const lock = std::scoped_lock{statePtr->mutex};
        return statePtr->cachedDevices;
      }

      std::shared_ptr<detail::WasapiGraphRegistry> graphRegistry() const { return _graphRegistryPtr; }

    private:
      enum class Lifecycle : std::uint8_t
      {
        Running,
        Stopping,
        Stopped,
      };

      class [[nodiscard]] CallbackScope final
      {
      public:
        explicit CallbackScope(Control& control)
          : _control{control}
        {
          auto const lock = std::scoped_lock{_control._lifecycleMutex};

          auto const callbackAlreadyActive = utility::CallbackStackScope::containsIdentity(&_control);

          if (_control._lifecycle == Lifecycle::Running ||
              (_control._lifecycle == Lifecycle::Stopping && !callbackAlreadyActive))
          {
            ++_control._activeCallbackCount;
            _optCallbackStackScope.emplace(&_control);
          }
        }

        ~CallbackScope()
        {
          if (!_optCallbackStackScope)
          {
            return;
          }

          _optCallbackStackScope.reset();
          auto const lock = std::scoped_lock{_control._lifecycleMutex};
          --_control._activeCallbackCount;
          _control.updateCompletionLocked();
        }

        CallbackScope(CallbackScope const&) = delete;
        CallbackScope& operator=(CallbackScope const&) = delete;
        CallbackScope(CallbackScope&&) = delete;
        CallbackScope& operator=(CallbackScope&&) = delete;

        bool isAdmitted() const noexcept { return _optCallbackStackScope.has_value(); }

      private:
        Control& _control;
        std::optional<utility::CallbackStackScope> _optCallbackStackScope;
      };

      bool acceptsSubscriptions() const
      {
        auto const lock = std::scoped_lock{_lifecycleMutex};
        return _lifecycle == Lifecycle::Running;
      }

      bool isCurrentCallback() const noexcept { return utility::CallbackStackScope::containsIdentity(this); }

      void invokeDeviceCallback(OnDevicesChangedCallback const& callback, std::vector<Device> const& devices)
      {
        auto const scope = CallbackScope{*this};

        if (scope.isAdmitted())
        {
          callback(devices);
        }
      }

      void invokeGraphCallback(OnGraphChangedCallback const& callback, flow::Graph const& graph)
      {
        auto const scope = CallbackScope{*this};

        if (scope.isAdmitted())
        {
          callback(graph);
        }
      }

      void markMonitorStarted() noexcept
      {
        auto const lock = std::scoped_lock{_lifecycleMutex};
        _monitorThreadId = std::this_thread::get_id();
      }

      void markMonitorExited() noexcept
      {
        auto const lock = std::scoped_lock{_lifecycleMutex};
        _monitorThreadId = {};
        _monitorExited = true;
        updateCompletionLocked();
      }

      void updateCompletionLocked() noexcept
      {
        if (_lifecycle == Lifecycle::Stopping && _registriesRetired && _monitorExited && _activeCallbackCount == 0U)
        {
          _lifecycle = Lifecycle::Stopped;
          _completionChanged.notify_all();
        }
      }

      void waitForCompletion() noexcept
      {
        auto lock = std::unique_lock{_lifecycleMutex};
        _completionChanged.wait(lock, [this] { return _lifecycle == Lifecycle::Stopped; });
      }

      void notifyShutdownStarted() const noexcept
      {
        if (_monitorStatePtr->monitorHooksPtr)
        {
          invokeHook(_monitorStatePtr->monitorHooksPtr->onShutdownStarted, "WASAPI shutdown-start observer");
        }
      }

      void notifyShutdownWait() const noexcept
      {
        if (_monitorStatePtr->monitorHooksPtr)
        {
          invokeHook(_monitorStatePtr->monitorHooksPtr->onShutdownWait, "WASAPI shutdown-wait observer");
        }
      }

      std::shared_ptr<detail::WasapiGraphRegistry> _graphRegistryPtr;
      std::shared_ptr<MonitorState> _monitorStatePtr;
      mutable std::mutex _lifecycleMutex;
      std::condition_variable _completionChanged;
      std::jthread _monitorThread;
      std::thread::id _monitorThreadId{};
      std::size_t _activeCallbackCount = 0U;
      Lifecycle _lifecycle = Lifecycle::Running;
      bool _registriesRetired = false;
      bool _monitorExited = false;
    };

    std::shared_ptr<Control> controlPtr;

    explicit Impl(std::shared_ptr<detail::WasapiProviderMonitorHooks> monitorHooksPtr)
      : controlPtr{std::make_shared<Control>(std::move(monitorHooksPtr))}
    {
      controlPtr->startMonitor();
    }

    ~Impl()
    {
      auto const retainedControlPtr = controlPtr;
      retainedControlPtr->shutdown();
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
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
    auto const controlPtr = _implPtr->controlPtr;
    controlPtr->shutdown();
  }

  Subscription WasapiProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    auto const controlPtr = _implPtr->controlPtr;
    return controlPtr->subscribeDevices(std::move(callback));
  }

  BackendProvider::Status WasapiProvider::status() const
  {
    auto const controlPtr = _implPtr->controlPtr;
    return {.descriptor = {.id = kBackendWasapi, .supportedProfiles = {{.id = kProfileShared}}},
            .devices = controlPtr->devices()};
  }

  std::unique_ptr<Backend> WasapiProvider::createBackend(Device const& device, ProfileId const& /*profile*/)
  {
    auto const controlPtr = _implPtr->controlPtr;
    return std::make_unique<WasapiSharedBackend>(device, kProfileShared, controlPtr->graphRegistry());
  }

  Subscription WasapiProvider::subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback)
  {
    auto const controlPtr = _implPtr->controlPtr;
    return controlPtr->subscribeGraph(routeAnchor, std::move(callback));
  }
} // namespace ao::audio::backend

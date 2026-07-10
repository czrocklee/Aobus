// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/backend/WasapiSharedBackend.h>
#include <ao/audio/backend/detail/AudioBackendRenderProgress.h>
#include <ao/audio/backend/detail/WasapiFormat.h>
#include <ao/audio/backend/detail/WasapiGraphRegistry.h>
#include <ao/audio/backend/detail/WasapiRenderBuffer.h>
#include <ao/audio/backend/detail/WasapiStrings.h>
#include <ao/utility/ThreadName.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <audioclient.h>
#include <avrt.h>
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace ao::audio::backend
{
  namespace
  {
    using Microsoft::WRL::ComPtr;

    constexpr auto kRenderWaitTimeout = std::chrono::milliseconds{500};
    constexpr auto kIdleRetryDelay = std::chrono::milliseconds{1};
    constexpr auto kPausedPollDelay = std::chrono::milliseconds{10};
    constexpr auto kDrainPollDelay = std::chrono::milliseconds{10};

    std::string describeHresult(HRESULT const hr)
    {
      return std::format("HRESULT {:#010x}", static_cast<std::uint32_t>(hr));
    }

    DWORD channelMaskFor(std::uint8_t const channels) noexcept
    {
      switch (channels)
      {
        case 1: return SPEAKER_FRONT_CENTER;
        case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        // 0 lets the audio engine pick its default mapping for the count.
        default: return 0;
      }
    }

    WAVEFORMATEXTENSIBLE toWaveFormat(Format const& format) noexcept
    {
      auto const containerBytes = bytesPerSample(format);

      auto wave = WAVEFORMATEXTENSIBLE{};
      wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      wave.Format.nChannels = format.channels;
      wave.Format.nSamplesPerSec = format.sampleRate;
      wave.Format.wBitsPerSample = static_cast<WORD>(containerBytes * 8U);
      wave.Format.nBlockAlign = static_cast<WORD>(static_cast<std::uint32_t>(format.channels) * containerBytes);
      wave.Format.nAvgBytesPerSec = format.sampleRate * wave.Format.nBlockAlign;
      wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
      // WAVEFORMATEXTENSIBLE exposes valid bits through its C ABI union.
      wave.Samples.wValidBitsPerSample = effectiveBits(format); // NOLINT(cppcoreguidelines-pro-type-union-access)
      wave.dwChannelMask = channelMaskFor(format.channels);
      wave.SubFormat = format.isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
      return wave;
    }

    /**
     * @brief Keeps the process-wide COM multithreaded apartment alive.
     *
     * Threads that never call CoInitializeEx become implicit members of the
     * MTA while this usage is held, so the render and control threads can use
     * the WASAPI interfaces without per-thread apartment management.
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
  } // namespace

  std::optional<Format> detail::formatFromWaveFormat(WAVEFORMATEX const& wave) noexcept
  {
    if (wave.nSamplesPerSec == 0 || wave.nChannels == 0 || wave.nChannels > std::numeric_limits<std::uint8_t>::max() ||
        wave.wBitsPerSample == 0 || wave.wBitsPerSample > std::numeric_limits<std::uint8_t>::max())
    {
      return std::nullopt;
    }

    bool isFloat = false;
    auto validBits = wave.wBitsPerSample;

    if (wave.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
      isFloat = true;
    }
    else if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
      if (wave.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
      {
        return std::nullopt;
      }

      // The wFormatTag/cbSize checks establish that the external object is WAVEFORMATEXTENSIBLE.
      auto const& extensible =
        reinterpret_cast<WAVEFORMATEXTENSIBLE const&>(wave); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)

      if (::IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE)
      {
        isFloat = true;
      }
      else if (::IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) == FALSE)
      {
        return std::nullopt;
      }

      // Samples is the Windows SDK union carrying the active valid-bits field.
      validBits = extensible.Samples.wValidBitsPerSample; // NOLINT(cppcoreguidelines-pro-type-union-access)
    }
    else if (wave.wFormatTag != WAVE_FORMAT_PCM)
    {
      return std::nullopt;
    }

    if (validBits > wave.wBitsPerSample || validBits > std::numeric_limits<std::uint8_t>::max())
    {
      return std::nullopt;
    }

    return Format{.sampleRate = wave.nSamplesPerSec,
                  .channels = static_cast<std::uint8_t>(wave.nChannels),
                  .bitDepth = static_cast<std::uint8_t>(wave.wBitsPerSample),
                  .validBits = static_cast<std::uint8_t>(validBits),
                  .isFloat = isFloat,
                  .isInterleaved = true};
  }

  struct WasapiSharedBackend::Impl final
  {
    MtaUsage mtaUsage;

    std::string deviceId; // UTF-8 endpoint ID; empty selects the default endpoint
    std::string routeAnchor;
    Format format;
    std::optional<Format> optMixFormat;
    RenderTarget* renderTarget = nullptr;

    ComPtr<IAudioClient> audioClient;
    ComPtr<IAudioRenderClient> renderClient;
    HANDLE renderEvent = nullptr;
    UINT32 bufferFrames = 0;
    std::size_t bytesPerFrame = 0;

    std::jthread thread;
    std::atomic<bool> paused{false};
    mutable std::atomic<bool> fatalStreamError{false};

    // Serializes ISimpleAudioVolume access and the cached fallbacks between the
    // control-path property methods and open()/close(). The render loop never
    // touches the session interface, so it does not contend here.
    mutable std::mutex sessionMutex;
    ComPtr<ISimpleAudioVolume> sessionVolume;
    float cachedVolume = 1.0F;
    bool cachedMuted = false;
    std::optional<float> optAppliedVolume;
    std::optional<bool> optAppliedMuted;

    std::shared_ptr<detail::WasapiGraphRegistry> graphRegistryPtr;

    explicit Impl(std::string id, std::shared_ptr<detail::WasapiGraphRegistry> graphRegistryPtr)
      : deviceId{std::move(id)}, routeAnchor{deviceId}, graphRegistryPtr{std::move(graphRegistryPtr)}
    {
    }

    void renderLoop(std::stop_token const& stopToken);
    bool syncPauseState(bool& devicePaused, bool started) const;
    bool renderOnce(std::stop_token const& stopToken, bool& started) const;
    void drainAndComplete(std::stop_token const& stopToken, bool& started) const;
    void failStream(std::string message) const;

    void publishGraphState();
  };

  void WasapiSharedBackend::Impl::renderLoop(std::stop_token const& stopToken)
  {
    setCurrentThreadName("WasapiRender");

    // Ask MMCSS to schedule this thread like other pro-audio render threads.
    // Failure is harmless; playback just runs at normal priority.
    DWORD taskIndex = 0;
    auto* const mmcssHandle = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    // Tracks the device-side run state owned exclusively by this thread.
    // pause()/resume() only flip the `paused` atomic; the edge is applied here.
    bool devicePaused = false;

    // The stream is started only after the first buffer is committed, so the
    // engine never renders from a fully empty buffer at startup.
    bool started = false;

    while (!stopToken.stop_requested() && !fatalStreamError.load(std::memory_order_relaxed))
    {
      if (!syncPauseState(devicePaused, started))
      {
        break;
      }

      if (devicePaused)
      {
        std::this_thread::sleep_for(kPausedPollDelay);
        continue;
      }

      if (started)
      {
        // Render events only fire while the stream is running; before Start()
        // the buffer is filled without waiting.
        auto const waitResult = ::WaitForSingleObject(renderEvent, static_cast<DWORD>(kRenderWaitTimeout.count()));

        if (waitResult == WAIT_TIMEOUT)
        {
          continue;
        }

        if (waitResult != WAIT_OBJECT_0)
        {
          failStream(std::format("WASAPI: render event wait failed (Win32 error {})", ::GetLastError()));
          break;
        }
      }

      if (stopToken.stop_requested())
      {
        break;
      }

      if (!renderOnce(stopToken, started))
      {
        break;
      }
    }

    if (mmcssHandle != nullptr)
    {
      ::AvRevertMmThreadCharacteristics(mmcssHandle);
    }
  }

  bool WasapiSharedBackend::Impl::syncPauseState(bool& devicePaused, bool const started) const
  {
    // IAudioClient::Stop keeps the buffered data, so Stop/Start is an exact
    // pause/resume. Applied on this thread only to keep run-state transitions
    // single-threaded, mirroring the ALSA backend.
    bool const wantPaused = paused.load(std::memory_order_relaxed);

    if (wantPaused != devicePaused)
    {
      if (started)
      {
        auto const hr = wantPaused ? audioClient->Stop() : audioClient->Start();

        if (FAILED(hr))
        {
          failStream(std::format("WASAPI: {} failed while {} ({})",
                                 wantPaused ? "Stop" : "Start",
                                 wantPaused ? "pausing" : "resuming",
                                 describeHresult(hr)));
          return false;
        }
      }

      devicePaused = wantPaused;
    }

    return true;
  }

  bool WasapiSharedBackend::Impl::renderOnce(std::stop_token const& stopToken, bool& started) const
  {
    UINT32 padding = 0; // NOLINT(misc-const-correctness) -- COM writes this output value

    if (auto const hr = audioClient->GetCurrentPadding(&padding); FAILED(hr))
    {
      failStream(std::format("WASAPI: GetCurrentPadding failed ({})", describeHresult(hr)));
      return false;
    }

    if (padding == bufferFrames)
    {
      return true;
    }

    auto const framesAvailable = bufferFrames - padding;

    BYTE* data = nullptr; // NOLINT(misc-const-correctness) -- WASAPI returns a writable render buffer

    if (auto const hr = renderClient->GetBuffer(framesAvailable, &data); FAILED(hr))
    {
      failStream(std::format("WASAPI: GetBuffer failed ({})", describeHresult(hr)));
      return false;
    }

    // IAudioRenderClient returns writable bytes through the SDK's BYTE pointer.
    auto const output =
      std::span<std::byte>{reinterpret_cast<std::byte*>(data), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                           static_cast<std::size_t>(framesAvailable) * bytesPerFrame};
    auto const renderResult = renderTarget->renderPcm(output);
    auto const packet = detail::prepareWasapiRenderPacket(output, bytesPerFrame, renderResult);

    if (auto const hr = renderClient->ReleaseBuffer(packet.framesToRelease, 0); FAILED(hr))
    {
      failStream(std::format("WASAPI: ReleaseBuffer failed ({})", describeHresult(hr)));
      return false;
    }

    if (packet.framesToRelease > 0)
    {
      if (!started)
      {
        if (auto const hr = audioClient->Start(); FAILED(hr))
        {
          failStream(std::format("WASAPI: Start failed ({})", describeHresult(hr)));
          return false;
        }

        started = true;
      }

      if (packet.underrun)
      {
        renderTarget->handleUnderrun();
      }

      auto const committedPositionFrames = detail::committedPositionFrames(
        packet.renderedFrames, renderResult.positionFrameOffset, renderResult.positionFrames);

      if (committedPositionFrames > 0)
      {
        renderTarget->handlePositionAdvanced(committedPositionFrames);
      }
    }

    if (packet.drained)
    {
      drainAndComplete(stopToken, started);
      return false;
    }

    if (packet.framesToRelease == 0)
    {
      std::this_thread::sleep_for(kIdleRetryDelay);
    }

    return true;
  }

  void WasapiSharedBackend::Impl::drainAndComplete(std::stop_token const& stopToken, bool& started) const
  {
    // Let the frames already committed to the endpoint buffer play out before
    // reporting drain completion (WASAPI has no equivalent of snd_pcm_drain).
    while (started && !stopToken.stop_requested())
    {
      UINT32 padding = 0; // NOLINT(misc-const-correctness) -- COM writes this output value

      if (auto const hr = audioClient->GetCurrentPadding(&padding); FAILED(hr))
      {
        failStream(std::format("WASAPI: GetCurrentPadding failed while draining ({})", describeHresult(hr)));
        return;
      }

      if (padding == 0)
      {
        break;
      }

      std::this_thread::sleep_for(kDrainPollDelay);
    }

    if (stopToken.stop_requested())
    {
      return;
    }

    if (started)
    {
      if (auto const hr = audioClient->Stop(); FAILED(hr))
      {
        failStream(std::format("WASAPI: Stop failed after draining ({})", describeHresult(hr)));
        return;
      }

      started = false;
    }

    renderTarget->handleDrainComplete();
  }

  void WasapiSharedBackend::Impl::failStream(std::string message) const
  {
    if (!fatalStreamError.exchange(true, std::memory_order_relaxed) && renderTarget != nullptr)
    {
      renderTarget->handleBackendError(message);
    }
  }

  void WasapiSharedBackend::Impl::publishGraphState()
  {
    if (!graphRegistryPtr)
    {
      return;
    }

    auto state = detail::WasapiRouteState{.routeAnchor = routeAnchor};

    {
      auto const lock = std::scoped_lock{sessionMutex};
      state.volume = optAppliedVolume.value_or(1.0F);
      state.muted = optAppliedMuted.value_or(false);
    }

    if (audioClient.Get() != nullptr)
    {
      state.optInputFormat = format;
      state.optMixFormat = optMixFormat;
    }

    graphRegistryPtr->publish(std::move(state));
  }

  WasapiSharedBackend::WasapiSharedBackend(Device const& device, ProfileId const& /*profile*/)
    : _implPtr{std::make_unique<Impl>(device.id.raw(), nullptr)}
  {
  }

  WasapiSharedBackend::WasapiSharedBackend(Device const& device,
                                           ProfileId const& /*profile*/,
                                           std::shared_ptr<detail::WasapiGraphRegistry> graphRegistryPtr)
    : _implPtr{std::make_unique<Impl>(device.id.raw(), std::move(graphRegistryPtr))}
  {
  }

  WasapiSharedBackend::~WasapiSharedBackend()
  {
    try
    {
      close();
    }
    catch (...) // NOLINT(bugprone-empty-catch) -- cleanup callbacks cannot escape a destructor
    {
    }
  }

  Result<> WasapiSharedBackend::open(Format const& format, RenderTarget* target)
  {
    close();

    _implPtr->format = format;
    _implPtr->renderTarget = target;
    _implPtr->paused.store(false, std::memory_order_relaxed);
    _implPtr->fatalStreamError.store(false, std::memory_order_relaxed);

    auto enumerator = ComPtr<IMMDeviceEnumerator>{};

    if (auto const hr =
          ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        FAILED(hr))
    {
      return makeError(
        Error::Code::InitFailed, std::format("Failed to create MMDeviceEnumerator ({})", describeHresult(hr)));
    }

    auto device = ComPtr<IMMDevice>{};

    if (_implPtr->deviceId.empty())
    {
      if (auto const hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device); FAILED(hr))
      {
        return makeError(
          Error::Code::DeviceNotFound, std::format("No default WASAPI render endpoint ({})", describeHresult(hr)));
      }
    }
    else if (auto const hr = enumerator->GetDevice(detail::utf8ToWide(_implPtr->deviceId).c_str(), &device); FAILED(hr))
    {
      return makeError(Error::Code::DeviceNotFound,
                       std::format("Failed to open WASAPI endpoint {} ({})", _implPtr->deviceId, describeHresult(hr)));
    }

    // Resolve the actual endpoint ID so the route anchor is stable even when
    // the default endpoint was requested.
    {
      if (LPWSTR rawId = nullptr; SUCCEEDED(device->GetId(&rawId)) && rawId != nullptr)
      {
        _implPtr->routeAnchor = detail::wideToUtf8(rawId);
        ::CoTaskMemFree(rawId);
      }
    }

    auto audioClient = ComPtr<IAudioClient>{};

    if (auto const hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient); FAILED(hr))
    {
      return makeError(
        Error::Code::InitFailed, std::format("Failed to activate IAudioClient ({})", describeHresult(hr)));
    }

    auto const wave = toWaveFormat(format);
    constexpr DWORD kStreamFlags =
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    auto optMixFormat = std::optional<Format>{};
    WAVEFORMATEX* rawMixFormat = nullptr;

    if (SUCCEEDED(audioClient->GetMixFormat(&rawMixFormat)) && rawMixFormat != nullptr)
    {
      optMixFormat = detail::formatFromWaveFormat(*rawMixFormat);
    }

    if (rawMixFormat != nullptr)
    {
      ::CoTaskMemFree(rawMixFormat);
    }

    // Shared event-driven streams require both duration arguments to be zero;
    // the audio engine selects a period compatible with the endpoint.
    if (auto const hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, kStreamFlags, 0, 0, &wave.Format, nullptr);
        FAILED(hr))
    {
      auto const code = (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) ? Error::Code::FormatRejected : Error::Code::InitFailed;
      return makeError(code, std::format("IAudioClient::Initialize failed ({})", describeHresult(hr)));
    }

    if (auto const hr = audioClient->GetBufferSize(&_implPtr->bufferFrames); FAILED(hr))
    {
      return makeError(Error::Code::InitFailed, std::format("GetBufferSize failed ({})", describeHresult(hr)));
    }

    auto* const renderEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (renderEvent == nullptr)
    {
      return makeError(Error::Code::InitFailed, "Failed to create WASAPI render event");
    }

    if (auto const hr = audioClient->SetEventHandle(renderEvent); FAILED(hr))
    {
      ::CloseHandle(renderEvent);
      return makeError(Error::Code::InitFailed, std::format("SetEventHandle failed ({})", describeHresult(hr)));
    }

    auto renderClient = ComPtr<IAudioRenderClient>{};

    if (auto const hr = audioClient->GetService(IID_PPV_ARGS(&renderClient)); FAILED(hr))
    {
      ::CloseHandle(renderEvent);
      return makeError(
        Error::Code::InitFailed, std::format("Failed to obtain IAudioRenderClient ({})", describeHresult(hr)));
    }

    auto sessionVolume = ComPtr<ISimpleAudioVolume>{};
    audioClient->GetService(IID_PPV_ARGS(&sessionVolume)); // volume control is best-effort

    if (sessionVolume.Get() != nullptr)
    {
      float cachedVolume = 1.0F;
      bool cachedMuted = false;

      {
        auto const lock = std::scoped_lock{_implPtr->sessionMutex};
        cachedVolume = _implPtr->cachedVolume;
        cachedMuted = _implPtr->cachedMuted;
      }

      if (auto const hr = sessionVolume->SetMasterVolume(cachedVolume, nullptr); FAILED(hr))
      {
        ::CloseHandle(renderEvent);
        return makeError(
          Error::Code::IoError, std::format("Failed to restore WASAPI session volume ({})", describeHresult(hr)));
      }

      if (auto const hr = sessionVolume->SetMute(cachedMuted ? TRUE : FALSE, nullptr); FAILED(hr))
      {
        ::CloseHandle(renderEvent);
        return makeError(
          Error::Code::IoError, std::format("Failed to restore WASAPI session mute ({})", describeHresult(hr)));
      }
    }

    _implPtr->bytesPerFrame = frameBytes(format);
    _implPtr->optMixFormat = optMixFormat;
    _implPtr->renderEvent = renderEvent;
    _implPtr->audioClient = std::move(audioClient);
    _implPtr->renderClient = std::move(renderClient);

    {
      auto const lock = std::scoped_lock{_implPtr->sessionMutex};
      _implPtr->sessionVolume = std::move(sessionVolume);
      _implPtr->optAppliedVolume =
        _implPtr->sessionVolume.Get() != nullptr ? std::optional{_implPtr->cachedVolume} : std::nullopt;
      _implPtr->optAppliedMuted =
        _implPtr->sessionVolume.Get() != nullptr ? std::optional{_implPtr->cachedMuted} : std::nullopt;
    }

    // The input stays in the negotiated decoder-side format. The graph records
    // the separately queried endpoint mix format when Windows exposes one.
    _implPtr->renderTarget->handleRouteReady(_implPtr->routeAnchor);

    _implPtr->publishGraphState();

    return {};
  }

  void WasapiSharedBackend::start()
  {
    if (_implPtr->audioClient.Get() == nullptr)
    {
      return;
    }

    _implPtr->paused.store(false, std::memory_order_relaxed);

    if (!_implPtr->thread.joinable())
    {
      _implPtr->thread = std::jthread{[this](std::stop_token const& st) { _implPtr->renderLoop(st); }};
    }

    // The stream itself is started from the render thread once the first
    // buffer has been committed; IAudioClient is never touched here.
  }

  void WasapiSharedBackend::pause()
  {
    if (_implPtr->audioClient.Get() == nullptr)
    {
      return;
    }

    // Flip the intent only; the render loop applies IAudioClient::Stop on its
    // own thread to keep run-state transitions single-threaded.
    _implPtr->paused.store(true, std::memory_order_relaxed);
  }

  void WasapiSharedBackend::resume()
  {
    if (_implPtr->audioClient.Get() == nullptr)
    {
      return;
    }

    // Flip the intent only; the render loop applies IAudioClient::Start on its
    // own thread.
    _implPtr->paused.store(false, std::memory_order_relaxed);
  }

  void WasapiSharedBackend::flush()
  {
    // The render loop owns the stream's run state while it is running. Reuse
    // stop() as the quiescent point; it also resets the endpoint buffer.
    stop();
  }

  void WasapiSharedBackend::stop()
  {
    _implPtr->thread.request_stop();

    if (_implPtr->renderEvent != nullptr)
    {
      // Wake the render loop if it is blocked waiting for a device event.
      ::SetEvent(_implPtr->renderEvent);
    }

    if (_implPtr->thread.joinable() && std::this_thread::get_id() != _implPtr->thread.get_id())
    {
      _implPtr->thread.join();
    }

    if (_implPtr->audioClient.Get() != nullptr)
    {
      if (auto const hr = _implPtr->audioClient->Stop(); FAILED(hr))
      {
        _implPtr->failStream(std::format("WASAPI: Stop failed ({})", describeHresult(hr)));
      }

      if (auto const hr = _implPtr->audioClient->Reset(); FAILED(hr))
      {
        _implPtr->failStream(std::format("WASAPI: Reset failed ({})", describeHresult(hr)));
      }
    }

    _implPtr->paused.store(false, std::memory_order_relaxed);
  }

  void WasapiSharedBackend::close()
  {
    if (_implPtr->graphRegistryPtr)
    {
      _implPtr->graphRegistryPtr->clear(_implPtr->routeAnchor);
    }

    stop();

    {
      auto const lock = std::scoped_lock{_implPtr->sessionMutex};
      _implPtr->sessionVolume.Reset();
      _implPtr->optAppliedVolume.reset();
      _implPtr->optAppliedMuted.reset();
    }

    _implPtr->renderClient.Reset();
    _implPtr->audioClient.Reset();
    _implPtr->optMixFormat.reset();

    if (_implPtr->renderEvent != nullptr)
    {
      ::CloseHandle(_implPtr->renderEvent);
      _implPtr->renderEvent = nullptr;
    }

    _implPtr->renderTarget = nullptr;
  }

  Result<> WasapiSharedBackend::setProperty(PropertyId id, PropertyValue const& value)
  {
    if (id == PropertyId::Volume)
    {
      auto const volume = std::clamp(std::get<float>(value), 0.0F, 1.0F);

      {
        auto const lock = std::scoped_lock{_implPtr->sessionMutex};

        if (_implPtr->sessionVolume.Get() != nullptr)
        {
          if (auto const hr = _implPtr->sessionVolume->SetMasterVolume(volume, nullptr); FAILED(hr))
          {
            return makeError(
              Error::Code::IoError, std::format("WASAPI: failed to set session volume ({})", describeHresult(hr)));
          }

          _implPtr->optAppliedVolume = volume;
        }
        else if (_implPtr->audioClient.Get() != nullptr)
        {
          return makeError(Error::Code::NotSupported, "WASAPI session volume is unavailable");
        }

        _implPtr->cachedVolume = volume;
      }

      _implPtr->publishGraphState();
      return {};
    }

    if (id == PropertyId::Muted)
    {
      auto const muted = std::get<bool>(value);

      {
        auto const lock = std::scoped_lock{_implPtr->sessionMutex};

        if (_implPtr->sessionVolume.Get() != nullptr)
        {
          if (auto const hr = _implPtr->sessionVolume->SetMute(muted ? TRUE : FALSE, nullptr); FAILED(hr))
          {
            return makeError(
              Error::Code::IoError, std::format("WASAPI: failed to set session mute ({})", describeHresult(hr)));
          }

          _implPtr->optAppliedMuted = muted;
        }
        else if (_implPtr->audioClient.Get() != nullptr)
        {
          return makeError(Error::Code::NotSupported, "WASAPI session mute is unavailable");
        }

        _implPtr->cachedMuted = muted;
      }

      _implPtr->publishGraphState();
      return {};
    }

    return makeError(Error::Code::NotSupported);
  }

  Result<PropertyValue> WasapiSharedBackend::property(PropertyId id) const
  {
    if (id == PropertyId::Volume)
    {
      auto const lock = std::scoped_lock{_implPtr->sessionMutex};

      if (_implPtr->sessionVolume.Get() != nullptr)
      {
        float volume = 0.0F;

        if (auto const hr = _implPtr->sessionVolume->GetMasterVolume(&volume); FAILED(hr))
        {
          return makeError(
            Error::Code::IoError, std::format("WASAPI: failed to read session volume ({})", describeHresult(hr)));
        }

        return volume;
      }

      return _implPtr->cachedVolume;
    }

    if (id == PropertyId::Muted)
    {
      auto const lock = std::scoped_lock{_implPtr->sessionMutex};

      if (_implPtr->sessionVolume.Get() != nullptr)
      {
        BOOL muted = FALSE;

        if (auto const hr = _implPtr->sessionVolume->GetMute(&muted); FAILED(hr))
        {
          return makeError(
            Error::Code::IoError, std::format("WASAPI: failed to read session mute ({})", describeHresult(hr)));
        }

        return muted != FALSE;
      }

      return _implPtr->cachedMuted;
    }

    return makeError(Error::Code::NotSupported);
  }

  PropertyInfo WasapiSharedBackend::queryProperty(PropertyId id) const noexcept
  {
    if (id == PropertyId::Volume || id == PropertyId::Muted)
    {
      auto const lock = std::scoped_lock{_implPtr->sessionMutex};
      return {.canRead = true,
              .canWrite = true,
              .isAvailable = _implPtr->sessionVolume.Get() != nullptr,
              .emitsChangeNotifications = false,
              .isHardwareAssisted = false};
    }

    return {};
  }

  BackendId WasapiSharedBackend::backendId() const
  {
    return kBackendWasapi;
  }

  ProfileId WasapiSharedBackend::profileId() const
  {
    return kProfileShared;
  }
} // namespace ao::audio::backend

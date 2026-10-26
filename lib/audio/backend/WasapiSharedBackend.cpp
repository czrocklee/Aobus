// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "backend/WasapiSharedBackend.h"

#include "backend/detail/SpeakerSet.h"
#include "backend/detail/WasapiError.h"
#include "backend/detail/WasapiFormat.h"
#include "backend/detail/WasapiFormatObservation.h"
#include "backend/detail/WasapiGraphRegistry.h"
#include "backend/detail/WasapiRenderBuffer.h"
#include "backend/detail/WasapiStrings.h"
#include "detail/DecoderOutput.h"
#include "detail/OpenedModeValidation.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/Device.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
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
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
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

    bool hasRepresentableWaveGeometry(PcmFormat const& format) noexcept
    {
      auto const stride = frameBytes(format);
      return format.sampleRate != 0U && stride != 0U && format.sampleRate <= std::numeric_limits<DWORD>::max() / stride;
    }

    SampleEncoding observedWavePcmEncoding(WORD const containerBits, WORD const validBits, bool const isFloat) noexcept
    {
      if (isFloat && containerBits == 32U && validBits == 32U)
      {
        return SampleEncoding::Float32Le;
      }

      if (!isFloat && containerBits == 16U && validBits == 16U)
      {
        return SampleEncoding::Signed16Le;
      }

      if (!isFloat && containerBits == 24U && validBits == 24U)
      {
        return SampleEncoding::Signed24PackedLe;
      }

      if (!isFloat && containerBits == 32U && validBits == 24U)
      {
        return SampleEncoding::Signed24In32Le;
      }

      if (!isFloat && containerBits == 32U && validBits == 32U)
      {
        return SampleEncoding::Signed32Le;
      }

      return SampleEncoding::Unknown;
    }

    bool canAdmitSourceFormat(SignalFormat const& sourceFormat) noexcept
    {
      auto const optEncoding = ::ao::audio::detail::preferredLosslessPcmEncoding(sourceFormat);
      return optEncoding && detail::hasSupportedSourceSpeakerSet(sourceFormat) &&
             detail::waveFormatFromPcmFormat(pcmFormat(sourceFormat, *optEncoding)).has_value();
    }

    std::string describeHresult(HRESULT const hr)
    {
      return std::format("HRESULT {:#010x}", static_cast<std::uint32_t>(hr));
    }

    Result<detail::WaveFormatObservation> observeMixFormat(IAudioClient& client)
    {
      WAVEFORMATEX* rawMixFormat = nullptr;

      auto const status = client.GetMixFormat(&rawMixFormat);
      return detail::observeMixFormat(status, detail::WaveFormatPtr{rawMixFormat, &::CoTaskMemFree});
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

  std::optional<WAVEFORMATEXTENSIBLE> detail::waveFormatFromPcmFormat(PcmFormat const& format) noexcept
  {
    auto const containerBytes = bytesPerSample(format.encoding);
    auto const stride = frameBytes(format);

    if (!hasRepresentableWaveGeometry(format) || !hasSupportedSourceSpeakerSet(signalFormat(format)))
    {
      return std::nullopt;
    }

    auto wave = WAVEFORMATEXTENSIBLE{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = format.channels;
    wave.Format.nSamplesPerSec = format.sampleRate;
    wave.Format.wBitsPerSample = static_cast<WORD>(containerBytes * 8U);
    wave.Format.nBlockAlign = static_cast<WORD>(stride);
    wave.Format.nAvgBytesPerSec = static_cast<DWORD>(format.sampleRate * stride);
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    // WAVEFORMATEXTENSIBLE exposes valid bits through its C ABI union.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
    wave.Samples.wValidBitsPerSample = encodingNominalBits(format.encoding);

    // The canonical client format offers the exact source role set to
    // Initialize at every channel count; the source and the WAVE descriptor
    // both assign ascending speaker bits, so no byte permutation is needed.
    // Only a zero-mask source keeps the legacy defaults: count-derived
    // FC/FL-FR for one or two channels, and a direct-out (mask zero)
    // descriptor for higher counts. Zero never becomes a source identity.
    wave.dwChannelMask = format.speakerMask;

    if (wave.dwChannelMask == 0U && format.channels <= 2U)
    {
      wave.dwChannelMask = format.channels == 1U ? SPEAKER_FRONT_CENTER : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    }

    wave.SubFormat = isFloatEncoding(format.encoding) ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    return wave;
  }

  ChannelDeliveryEvidence detail::waveChannelDeliveryFromObservation(PcmFormat const& applicationFormat,
                                                                     WAVEFORMATEXTENSIBLE const& acceptedClient,
                                                                     WaveFormatObservation const& mix) noexcept
  {
    auto evidence = ChannelDeliveryEvidence{.nativeSpeakerMask = acceptedClient.dwChannelMask};

    // An exact accepted descriptor (nonzero application mask, equal counts,
    // equal role mask) marks the client boundary as mapped at every channel
    // count. This is the configuration Initialize accepted, not a claim of
    // endpoint physical wiring or initialized-client readback.
    if (applicationFormat.speakerMask != 0U && acceptedClient.Format.nChannels == applicationFormat.channels &&
        acceptedClient.dwChannelMask == applicationFormat.speakerMask)
    {
      evidence.isMapped = true;
    }

    if (mix.channels != 0U)
    {
      evidence.hasSharedConversion =
        acceptedClient.Format.nChannels != mix.channels ||
        (acceptedClient.dwChannelMask != 0U && mix.speakerMask != 0U &&
         std::cmp_greater(std::popcount(acceptedClient.dwChannelMask | mix.speakerMask), mix.channels));
    }

    return evidence;
  }

  namespace detail
  {
    Result<WaveFormatObservation> observeMixFormat(HRESULT const status, WaveFormatPtr wavePtr)
    {
      if (FAILED(status))
      {
        // A failed optional query supplies no independent mix evidence.
        return WaveFormatObservation{};
      }

      if (!wavePtr)
      {
        return makeError(Error::Code::FormatRejected, "WASAPI GetMixFormat succeeded without a descriptor");
      }

      return observeWaveFormat(*wavePtr);
    }

    Result<WaveFormatObservation> observeWaveFormat(WAVEFORMATEX const& wave)
    {
      // Storage integrity owns the refusal: a successful descriptor that is
      // too short to be WAVEFORMATEXTENSIBLE cannot be read past WAVEFORMATEX,
      // and absent numerics must not hide that fault.
      if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
          wave.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
      {
        return makeError(Error::Code::FormatRejected, "WASAPI GetMixFormat returned a short extensible descriptor");
      }

      if (wave.nSamplesPerSec == 0U || wave.nChannels == 0U)
      {
        // An optional observation without a rate or count is unavailable; it
        // guesses no geometry and does not veto the client descriptor.
        return WaveFormatObservation{};
      }

      auto observation = WaveFormatObservation{.sampleRate = wave.nSamplesPerSec, .channels = wave.nChannels};
      bool isFloat = false;
      auto validBits = wave.wBitsPerSample;

      if (wave.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
      {
        isFloat = true;
      }
      else if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
      {
        // The wFormatTag/cbSize checks establish that the external object is WAVEFORMATEXTENSIBLE.
        auto const& extensible =
          reinterpret_cast<WAVEFORMATEXTENSIBLE const&>(wave); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)

        if ((extensible.dwChannelMask & ~kWaveSpeakerMask) != 0U)
        {
          // Reserved or wildcard positions are never legal native roles. The
          // whole role set is unusable, so only numerical facts survive.
          observation.speakerMask = 0U;
        }
        else
        {
          observation.speakerMask = extensible.dwChannelMask;

          // WAVEFORMATEXTENSIBLE ignores excess most-significant positions.
          // Fewer positions than channels retain partial facts, not bad geometry.
          while (std::cmp_greater(std::popcount(observation.speakerMask), wave.nChannels))
          {
            observation.speakerMask &= ~std::bit_floor(observation.speakerMask);
          }
        }

        if (::IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE)
        {
          isFloat = true;
        }
        else if (::IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) == FALSE)
        {
          // Unknown and non-PCM subtypes own their frame and block semantics;
          // their independent facts never undergo PCM validation.
          return observation;
        }

        // Samples is the Windows SDK union carrying the active valid-bits field.
        validBits = extensible.Samples.wValidBitsPerSample; // NOLINT(cppcoreguidelines-pro-type-union-access)
      }
      else if (wave.wFormatTag != WAVE_FORMAT_PCM)
      {
        return observation;
      }

      auto const expectedStride = static_cast<std::uint64_t>(wave.nChannels) * (wave.wBitsPerSample / 8U);

      if (wave.wBitsPerSample == 0U || wave.wBitsPerSample % 8U != 0U || expectedStride != wave.nBlockAlign ||
          static_cast<std::uint64_t>(wave.nSamplesPerSec) * expectedStride != wave.nAvgBytesPerSec ||
          validBits > wave.wBitsPerSample)
      {
        // Contradictory optional arithmetic withholds the concrete format and
        // precision claim while the independent rate, count and role facts stay.
        return observation;
      }

      auto const encoding = observedWavePcmEncoding(wave.wBitsPerSample, validBits, isFloat);

      if (encoding != SampleEncoding::Unknown && wave.nChannels <= std::numeric_limits<std::uint8_t>::max())
      {
        observation.optFormat = PcmFormat{
          .sampleRate = wave.nSamplesPerSec,
          .channels = static_cast<std::uint8_t>(wave.nChannels),
          .encoding = encoding,
          .speakerMask =
            std::cmp_equal(std::popcount(observation.speakerMask), wave.nChannels) ? observation.speakerMask : 0U};
      }

      return observation;
    }
  } // namespace detail

  struct WasapiSharedBackend::Impl final
  {
    MtaUsage mtaUsage;

    std::string deviceId; // UTF-8 endpoint ID; empty selects the default endpoint
    std::string routeAnchor;
    PcmFormat nativeClientFormat;
    ChannelDeliveryEvidence channelDelivery;
    detail::WaveFormatObservation mixObservation;
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
    bool trySyncPauseState(bool& devicePaused, bool started) const;
    bool tryRenderOnce(std::stop_token const& stopToken, bool& started) const;
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
      if (!trySyncPauseState(devicePaused, started))
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

      if (!tryRenderOnce(stopToken, started))
      {
        break;
      }
    }

    if (mmcssHandle != nullptr)
    {
      ::AvRevertMmThreadCharacteristics(mmcssHandle);
    }
  }

  bool WasapiSharedBackend::Impl::trySyncPauseState(bool& devicePaused, bool const started) const
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

  bool WasapiSharedBackend::Impl::tryRenderOnce(std::stop_token const& stopToken, bool& started) const
  {
    UINT32 padding = 0; // NOLINT(misc-const-correctness) -- COM writes this output value

    if (auto const hr = audioClient->GetCurrentPadding(&padding); FAILED(hr))
    {
      failStream(std::format("WASAPI: GetCurrentPadding failed ({})", describeHresult(hr)));
      return false;
    }

    if (padding > bufferFrames)
    {
      failStream(
        std::format("WASAPI: endpoint reported {} padding frames for a {}-frame buffer", padding, bufferFrames));
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

      if (packet.positionFrames > 0)
      {
        renderTarget->handlePositionAdvanced(packet.positionFrames);
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
      state.optInputFormat = nativeClientFormat;
      state.mixObservation = mixObservation;
      state.channelDelivery = channelDelivery;
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
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "WASAPI backend destruction");
    }
  }

  std::optional<PcmFormat> WasapiSharedBackend::prewarmFormatHint(SignalFormat const& sourceFormat) const noexcept
  {
    return canAdmitSourceFormat(sourceFormat) ? Backend::prewarmFormatHint(sourceFormat) : std::nullopt;
  }

  bool WasapiSharedBackend::canReuseFormat(PcmFormat const& currentFormat, PcmFormat const& nextFormat) const noexcept
  {
    // Both stereo masks configure the same FL/FR descriptor and byte order.
    // Mix-format observations never replace that installed client transport.
    return Backend::canReuseFormat(currentFormat, nextFormat) ||
           ::ao::audio::detail::isStereoIdentityAlias(currentFormat, nextFormat);
  }

  BackendOpenResult WasapiSharedBackend::open(SignalFormat const& sourceFormat, RenderTarget& target)
  {
    // Reject deterministic source refusals before this backend's own close().
    // Engine closes its previous playback before calling open().
    if (sourceFormat.sampleRate == 0U || sourceFormat.channels == 0U)
    {
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::InvalidInput, "WASAPI PCM formats require a sample rate and channels").error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto const encodings = ::ao::audio::detail::losslessPcmEncodings(sourceFormat);

    if (encodings.empty())
    {
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::FormatRejected, "No lossless WASAPI input encoding is available").error(),
        .cause = BackendOpenFailure::Cause::TrackRouteIncompatible}};
    }

    auto const format = pcmFormat(sourceFormat, encodings.front());

    // The selected lossless container fixes byte geometry. Its representation
    // fault remains terminal even when the source also has an unsupported set.
    if (!hasRepresentableWaveGeometry(format))
    {
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::InvalidInput, "WASAPI PCM descriptor geometry is not representable").error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    // A complete supported set is the identified-source admission contract;
    // partial or reserved sets never reach a native descriptor.
    if (!detail::hasSupportedSourceSpeakerSet(sourceFormat))
    {
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::NotSupported, "WASAPI requires a complete supported speaker set").error(),
        .cause = BackendOpenFailure::Cause::TrackRouteIncompatible}};
    }

    auto const optWave = detail::waveFormatFromPcmFormat(format);
    AO_INVARIANT(optWave, "Validated WASAPI PCM descriptor must be representable");

    close();
    _implPtr->paused.store(false, std::memory_order_relaxed);
    _implPtr->fatalStreamError.store(false, std::memory_order_relaxed);

    auto enumerator = ComPtr<IMMDeviceEnumerator>{};

    if (auto const hr =
          ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        FAILED(hr))
    {
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::InitFailed,
                           std::format("Failed to create MMDeviceEnumerator ({})", describeHresult(hr)))
                   .error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto device = ComPtr<IMMDevice>{};

    if (_implPtr->deviceId.empty())
    {
      if (auto const hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device); FAILED(hr))
      {
        return std::unexpected{BackendOpenFailure{
          .error = makeError(Error::Code::DeviceNotFound,
                             std::format("No default WASAPI render endpoint ({})", describeHresult(hr)))
                     .error(),
          .cause = BackendOpenFailure::Cause::OutputFailure}};
      }
    }
    else if (auto const hr = enumerator->GetDevice(detail::utf8ToWide(_implPtr->deviceId).c_str(), &device); FAILED(hr))
    {
      return std::unexpected{BackendOpenFailure{
        .error =
          makeError(Error::Code::DeviceNotFound,
                    std::format("Failed to open WASAPI endpoint {} ({})", _implPtr->deviceId, describeHresult(hr)))
            .error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
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
      return std::unexpected{BackendOpenFailure{
        .error =
          makeError(Error::Code::InitFailed, std::format("Failed to activate IAudioClient ({})", describeHresult(hr)))
            .error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto const& wave = *optWave;
    constexpr DWORD kStreamFlags =
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    auto const mixObservationRes = observeMixFormat(*audioClient.Get());

    if (!mixObservationRes)
    {
      return std::unexpected{
        BackendOpenFailure{.error = mixObservationRes.error(), .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto const mixObservation = *mixObservationRes;

    // Shared event-driven streams require both duration arguments to be zero;
    // the audio engine selects a period compatible with the endpoint.
    if (auto const hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, kStreamFlags, 0, 0, &wave.Format, nullptr);
        FAILED(hr))
    {
      auto const code = (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) ? Error::Code::FormatRejected : Error::Code::InitFailed;
      return std::unexpected{BackendOpenFailure{
        .error = makeError(code, std::format("IAudioClient::Initialize failed ({})", describeHresult(hr))).error(),
        .cause = detail::wasapiInitializeFailureCause(hr)}};
    }

    if (auto const hr = audioClient->GetBufferSize(&_implPtr->bufferFrames); FAILED(hr))
    {
      return std::unexpected{BackendOpenFailure{
        .error =
          makeError(Error::Code::InitFailed, std::format("GetBufferSize failed ({})", describeHresult(hr))).error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto* const renderEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (renderEvent == nullptr)
    {
      return std::unexpected{
        BackendOpenFailure{.error = makeError(Error::Code::InitFailed, "Failed to create WASAPI render event").error(),
                           .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    if (auto const hr = audioClient->SetEventHandle(renderEvent); FAILED(hr))
    {
      ::CloseHandle(renderEvent);
      return std::unexpected{BackendOpenFailure{
        .error =
          makeError(Error::Code::InitFailed, std::format("SetEventHandle failed ({})", describeHresult(hr))).error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto renderClient = ComPtr<IAudioRenderClient>{};

    if (auto const hr = audioClient->GetService(IID_PPV_ARGS(&renderClient)); FAILED(hr))
    {
      ::CloseHandle(renderEvent);
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::InitFailed,
                           std::format("Failed to obtain IAudioRenderClient ({})", describeHresult(hr)))
                   .error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
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
        return std::unexpected{BackendOpenFailure{
          .error = makeError(Error::Code::IoError,
                             std::format("Failed to restore WASAPI session volume ({})", describeHresult(hr)))
                     .error(),
          .cause = BackendOpenFailure::Cause::OutputFailure}};
      }

      if (auto const hr = sessionVolume->SetMute(static_cast<::BOOL>(cachedMuted), nullptr); FAILED(hr))
      {
        ::CloseHandle(renderEvent);
        return std::unexpected{BackendOpenFailure{
          .error = makeError(Error::Code::IoError,
                             std::format("Failed to restore WASAPI session mute ({})", describeHresult(hr)))
                     .error(),
          .cause = BackendOpenFailure::Cause::OutputFailure}};
      }
    }

    _implPtr->bytesPerFrame = frameBytes(format);
    // Initialize accepted this actual descriptor; GetMixFormat is a separate
    // engine observation, not initialized-client readback or physical proof.
    _implPtr->nativeClientFormat = format;
    _implPtr->nativeClientFormat.speakerMask = wave.dwChannelMask;
    _implPtr->channelDelivery = detail::waveChannelDeliveryFromObservation(format, wave, mixObservation);
    _implPtr->mixObservation = mixObservation;
    _implPtr->renderEvent = renderEvent;
    _implPtr->audioClient = std::move(audioClient);
    _implPtr->renderClient = std::move(renderClient);
    _implPtr->renderTarget = &target;

    {
      auto const lock = std::scoped_lock{_implPtr->sessionMutex};
      _implPtr->sessionVolume = std::move(sessionVolume);
      _implPtr->optAppliedVolume =
        _implPtr->sessionVolume.Get() != nullptr ? std::optional<float>{_implPtr->cachedVolume} : std::nullopt;
      _implPtr->optAppliedMuted =
        _implPtr->sessionVolume.Get() != nullptr ? std::optional<bool>{_implPtr->cachedMuted} : std::nullopt;
    }

    // Capture the configured client boundary before outward notifications.
    // Client -> mix conversion is retained privately for the receiving sink,
    // not received by this client snapshot. No direct endpoint is confirmed.
    auto openedMode = OpenedPcmMode{.clientFormat = format, .channelDelivery = _implPtr->channelDelivery};
    openedMode.channelDelivery.hasSharedConversion = false;

    target.handleRouteReady(_implPtr->routeAnchor);
    _implPtr->publishGraphState();
    return openedMode;
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
      _implPtr->thread = std::jthread{[this](std::stop_token const& st)
                                      {
                                        try
                                        {
                                          _implPtr->renderLoop(st);
                                        }
                                        catch (...)
                                        {
                                          AO_FATAL_EXCEPTION(std::current_exception(), "WASAPI render thread");
                                        }
                                      }};
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
    _implPtr->mixObservation = {};
    _implPtr->nativeClientFormat = {};
    _implPtr->channelDelivery = {};

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

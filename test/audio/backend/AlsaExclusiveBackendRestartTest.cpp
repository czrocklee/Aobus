// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/AlsaExclusiveBackend.h"
#include "lib/audio/backend/detail/AlsaGraphRegistry.h"
#include "lib/audio/backend/detail/AlsaMixerSession.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace ao::audio::backend::test
{
  namespace
  {
    constexpr auto kPcmName = "aobus_alsa_restart_test";
    constexpr auto kCallbackTimeout = std::chrono::seconds{2};

    struct PlaybackObservation final
    {
      std::uint32_t positionFrames = 0;
      std::optional<std::string> optBackendError{};
    };

    class RecordingRenderTarget final : public RenderTarget
    {
    public:
      void supplyPositionFrames(std::uint32_t frames) noexcept { _nextPositionFrames.store(frames); }

      std::optional<PlaybackObservation> waitForObservation()
      {
        auto lock = std::unique_lock{_mutex};

        if (!_observationChanged.wait_for(lock, kCallbackTimeout, [this] { return _optObservation.has_value(); }))
        {
          return std::nullopt;
        }

        return std::exchange(_optObservation, std::nullopt);
      }

      RenderPcmResult renderPcm(std::span<std::byte> output) noexcept override
      {
        auto const frames = _nextPositionFrames.exchange(0);

        if (frames == 0)
        {
          return {};
        }

        std::ranges::fill(output, std::byte{0});
        return {.bytesWritten = output.size(), .positionFrames = frames};
      }

      void handlePositionAdvanced(std::uint32_t frames) noexcept override { publish({.positionFrames = frames}); }

      void handleUnderrun() noexcept override {}

      void handleDrainComplete() noexcept override {}

      void handleRouteReady(std::string_view /*routeAnchor*/) noexcept override {}

      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override {}

      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}

      void handleBackendError(std::string_view message) noexcept override
      {
        publish({.optBackendError = std::string{message}});
      }

    private:
      void publish(PlaybackObservation observation) noexcept
      {
        {
          auto const lock = std::scoped_lock{_mutex};

          if (!_optObservation)
          {
            _optObservation = std::move(observation);
          }
        }

        _observationChanged.notify_one();
      }

      std::atomic<std::uint32_t> _nextPositionFrames{0};
      std::mutex _mutex;
      std::condition_variable _observationChanged;
      std::optional<PlaybackObservation> _optObservation;
    };

    class NoMixerFactory final : public detail::AlsaMixerOpenFactory
    {
    public:
      std::unique_ptr<detail::AlsaMixerIo> open(::snd_pcm_t* /*pcm*/) override { return nullptr; }
    };

    void requirePositionAdvanced(RecordingRenderTarget& target, std::uint32_t expectedFrames)
    {
      CAPTURE(expectedFrames);
      auto const optObservation = target.waitForObservation();
      REQUIRE(optObservation);

      if (optObservation->optBackendError)
      {
        FAIL("ALSA backend error: " << *optObservation->optBackendError);
      }

      REQUIRE(optObservation->positionFrames == expectedFrames);
    }
  } // namespace

  // Limit ELF interposition to a private ALSA null PCM. The type override
  // admits that sink through the hardware-only check; ALSA still implements
  // negotiation, mmap, pause, drop and prepare for the actual playback worker.
  extern "C"
  {
    int nativePcmOpen(::snd_pcm_t** pcm,
                      char const* name,
                      ::snd_pcm_stream_t stream,
                      int mode) asm("__real_snd_pcm_open");
    int testPcmOpen(::snd_pcm_t** pcm,
                    char const* name,
                    ::snd_pcm_stream_t stream,
                    int mode) asm("__wrap_snd_pcm_open");
    ::snd_pcm_type_t nativePcmType(::snd_pcm_t* pcm) asm("__real_snd_pcm_type");
    ::snd_pcm_type_t testPcmType(::snd_pcm_t* pcm) asm("__wrap_snd_pcm_type");

    int testPcmOpen(::snd_pcm_t** pcm, char const* name, ::snd_pcm_stream_t stream, int mode)
    {
      if (std::string_view{name} != kPcmName)
      {
        return nativePcmOpen(pcm, name, stream, mode);
      }

      ::snd_config_t* config = nullptr;

      if (auto const status = ::snd_config_load_string(&config, "pcm.aobus_alsa_restart_test { type null }", 0);
          status < 0)
      {
        return status;
      }

      auto const status = ::snd_pcm_open_lconf(pcm, name, stream, mode, config);
      std::ignore = ::snd_config_delete(config);
      return status;
    }

    ::snd_pcm_type_t testPcmType(::snd_pcm_t* pcm)
    {
      return std::string_view{::snd_pcm_name(pcm)} == kPcmName ? SND_PCM_TYPE_HW : nativePcmType(pcm);
    }
  }

  TEST_CASE("AlsaExclusiveBackend - start advances playback after retiring a run with pause requested", "[concurrency]")
  {
    auto target = RecordingRenderTarget{};
    auto mixerFactory = NoMixerFactory{};
    auto const device =
      Device{.id = DeviceId{kPcmName}, .displayName = "Private ALSA null PCM", .backendId = kBackendAlsa};
    auto backend = AlsaExclusiveBackend{device,
                                        kProfileExclusive,
                                        detail::AlsaGraphPublisher{},
                                        std::make_unique<detail::AlsaMixerSession>(mixerFactory)};
    auto const source = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16};
    auto const requireOpen = [&]
    {
      auto const openedRes = backend.open(source, target);
      INFO((openedRes ? "opened" : openedRes.error().error.message));
      REQUIRE(openedRes);
    };
    requireOpen();

    target.supplyPositionFrames(7);
    backend.start();
    requirePositionAdvanced(target, 7);

    // Pause records intent synchronously; retiring the run need not wait for
    // the worker to apply the native pause edge.
    backend.pause();

    auto const requireRestartProgress = [&]
    {
      target.supplyPositionFrames(13);
      backend.start();
      requirePositionAdvanced(target, 13);
    };

    SECTION("restart on the retained stream after stop")
    {
      backend.stop();
      requireRestartProgress();
    }

    SECTION("restart after the stop and flush sequence used by a paused seek")
    {
      backend.stop();
      backend.flush();
      requireRestartProgress();
    }

    SECTION("restart after closing and opening again")
    {
      backend.close();
      requireOpen();
      requireRestartProgress();
    }
  }
} // namespace ao::audio::backend::test

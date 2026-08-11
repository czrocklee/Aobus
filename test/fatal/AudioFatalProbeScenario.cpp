// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/AudioFatalProbeScenario.h"

#include "lib/audio/detail/EngineEventQueueInvariants.h"
#include "lib/audio/detail/EngineRtSignalRing.h"
#include "lib/audio/detail/RenderTimeline.h"
#include "lib/audio/detail/TrackSession.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/audio/FakeCapturingBackend.h"
#include <ao/Error.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>

#ifdef __linux__
#include "lib/audio/backend/detail/AlsaGraphRegistry.h"
#endif
#ifdef _WIN32
#include "lib/audio/backend/WasapiProvider.h"
#include "lib/audio/backend/detail/WasapiGraphRegistry.h"
#include "lib/audio/backend/detail/WasapiProviderMonitorHooks.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    struct ProbeSignal final
    {
      std::uint32_t value = 0;
    };
  } // namespace

  std::int32_t runAudioFatalProbeScenario(std::string_view const scenario)
  {
    if (scenario == "rt-ring-overflow")
    {
      auto ring = detail::EngineRtSignalRing<ProbeSignal>{};
      ring.push({.value = 1});
      ring.push({.value = 2});
      ring.push({.value = 3});
    }

    if (scenario == "invalid-backend-third-push")
    {
      auto backendPtr = std::make_unique<FakeCapturingBackend>();
      auto* const backend = backendPtr.get();
      auto const format = PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le};
      auto engine = Engine{
        std::move(backendPtr),
        makeEngineTestDevice(),
        makePathScriptedDecoderFactory({
          {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = {std::byte{0x11}, std::byte{0x12}}},
          {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = {std::byte{0x21}, std::byte{0x22}}},
        })};

      engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
      auto const preparedRes = engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "second.flac"}));

      if (!preparedRes)
      {
        return 3;
      }

      auto eventWorkerEntered = std::binary_semaphore{0};
      auto holdEventWorker = std::binary_semaphore{0};
      engine.defer(
        [&]
        {
          eventWorkerEntered.release();
          holdEventWorker.acquire();
        });
      eventWorkerEntered.acquire();

      auto* const target = backend->target();

      if (target == nullptr)
      {
        return 3;
      }

      auto combinedOutput = std::array<std::byte, 4>{};
      [[maybe_unused]] auto const combinedResult = target->renderPcm(combinedOutput);

      auto drainedOutput = std::array<std::byte, 2>{};
      [[maybe_unused]] auto const firstDrainedResult = target->renderPcm(drainedOutput);
      backend->emitDrainComplete();

      // This fake deliberately violates Backend's drain-admission contract.
      // Rendering and completing the already drained source produces the third
      // pending signal and must abort at the queue boundary.
      [[maybe_unused]] auto const secondDrainedResult = target->renderPcm(drainedOutput);
      backend->emitDrainComplete();
    }

    if (scenario == "timeline-owner-overwrite")
    {
      auto timeline = detail::RenderTimeline{};
      timeline.armLookahead(std::make_unique<detail::RenderTimeline::Node>());
      [[maybe_unused]] auto* const consumedNode = timeline.consumeLookaheadForRender();
      timeline.armLookahead(std::make_unique<detail::RenderTimeline::Node>());
    }

    if (scenario == "decoder-factory-null-success")
    {
      auto const factory = [](std::filesystem::path const&,
                              std::optional<SampleEncoding>) -> Result<std::unique_ptr<DecoderSession>>
      { return std::unique_ptr<DecoderSession>{}; };
      [[maybe_unused]] auto const inspectionRes =
        detail::TrackSession::inspect(PlaybackInput{.filePath = "probe.flac"}, factory);
    }

    if (scenario == "queue-worker-joinable")
    {
      detail::verifyEngineEventQueueDestruction({.workerJoinable = true});
    }

    if (scenario == "queue-worker-running")
    {
      detail::verifyEngineEventQueueDestruction({.running = true});
    }

    if (scenario == "queue-playback-events")
    {
      detail::verifyEngineEventQueueDestruction({.playbackEventsEmpty = false});
    }

    if (scenario == "queue-rt-signals")
    {
      detail::verifyEngineEventQueueDestruction({.rtSignalCount = 1});
    }

    if (scenario == "engine-event-thread-exception")
    {
      auto engine =
        Engine{std::make_unique<FakeCapturingBackend>(), makeEngineTestDevice(), makePathScriptedDecoderFactory({})};
      auto callbackEntered = std::binary_semaphore{0};
      engine.defer(
        [&]
        {
          callbackEntered.release();
          throw std::runtime_error{"probe exception"};
        });
      callbackEntered.acquire();
    }

    if (scenario == "platform-graph-observer-exception")
    {
#ifdef __linux__
      auto registry = backend::detail::AlsaGraphRegistry{};
#else
      auto registry = backend::detail::WasapiGraphRegistry{};
#endif
      [[maybe_unused]] auto subscription =
        registry.subscribe("probe-route", [](auto const&) { throw std::runtime_error{"probe exception"}; });
    }

#ifdef _WIN32
    if (scenario == "wasapi-device-observer-exception")
    {
      auto hooksPtr = std::make_shared<backend::detail::WasapiProviderMonitorHooks>();
      hooksPtr->enumerateDevices = []
      { return std::vector<Device>{{.id = DeviceId{"probe-endpoint"}, .backendId = kBackendWasapi}}; };
      auto provider = backend::WasapiProvider{hooksPtr};
      auto callbackEntered = std::binary_semaphore{0};
      auto callbackCount = std::atomic_size_t{0};
      auto subscription = provider.subscribeDevices(
        [&](std::vector<Device> const&)
        {
          if (callbackCount.fetch_add(1, std::memory_order_relaxed) > 0)
          {
            callbackEntered.release();
            throw std::runtime_error{"probe exception"};
          }
        });

      if (!subscription || !hooksPtr->requestRefresh)
      {
        return 3;
      }

      hooksPtr->requestRefresh();
      callbackEntered.acquire();
    }
#endif

    return 2;
  }
} // namespace ao::audio::test

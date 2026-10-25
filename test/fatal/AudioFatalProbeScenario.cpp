// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/AudioFatalProbeScenario.h"

#include "lib/audio/backend/detail/BackendGraphRegistry.h"
#include "lib/audio/backend/detail/CallbackFence.h"
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

#ifdef _WIN32
#include "lib/audio/backend/WasapiProvider.h"
#include "lib/audio/backend/detail/WasapiProviderMonitorHooks.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>

#include <atomic>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio> // NOLINT(misc-include-cleaner): Direct provider of stderr.
#include <filesystem>
#include <memory>
#include <optional>
#include <print>
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

    void reportProbeFailure(std::string_view const scenario, std::string_view const reason)
    {
      std::println(stderr, "audio fatal probe '{}': {}", scenario, reason);
    }

    std::int32_t runCallbackFenceFatalProbe(std::string_view const scenario)
    {
      // Deterministic public-API preconditions on a single thread: open the
      // fence, then reopen it either while it is still open or after close()
      // with an admitted borrow still outstanding. The invalid reopen must
      // abort at the quiescence invariant. This is not the historical
      // concurrent fetch_add/undo race window; no timing is involved.
      auto fence = backend::detail::CallbackFence{};
      fence.open();

      if (scenario == "callback-fence-reopen-with-admitted-borrow")
      {
        if (!fence.tryEnter())
        {
          reportProbeFailure(scenario, "tryEnter setup failed to admit the probe borrow");
          return 3;
        }

        // close() only: closeAndWait() would restore quiescence and make the
        // reopen legal.
        fence.close();
      }

      fence.open();

      reportProbeFailure(scenario, "reopen returned without the expected abort");
      return 3;
    }
  } // namespace

  std::int32_t runAudioFatalProbeScenario(std::string_view const scenario)
  {
#ifdef __APPLE__
    if (scenario.starts_with("coreaudio-"))
    {
      return runCoreAudioFatalProbeScenario(scenario);
    }
#endif

    if (scenario == "callback-fence-reopen-while-open" || scenario == "callback-fence-reopen-with-admitted-borrow")
    {
      return runCallbackFenceFatalProbe(scenario);
    }

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
      auto registry = backend::detail::BackendGraphRegistry{};
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

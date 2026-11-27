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
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

    struct CallbackCounts final
    {
      std::uint32_t backendErrors = 0;
      std::uint32_t underruns = 0;
      std::uint32_t renderCalls = 0;
      std::uint32_t positionCallbacks = 0;
    };

    class RecordingRenderTarget final : public RenderTarget
    {
    public:
      void drainWhenEmpty() noexcept { _drainWhenEmpty = true; }

      // Read on the worker or after it exits.
      CallbackCounts counts() const noexcept { return _counts; }

      // Supply the entire script before starting the worker.
      void supplyPositionFrames(std::uint32_t first, std::uint32_t following = 0) noexcept
      {
        _nextPositionFrames = first;
        _followingPositionFrames = following;
      }

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
        ++_counts.renderCalls;
        auto const frames = std::exchange(_nextPositionFrames, std::exchange(_followingPositionFrames, 0U));

        if (frames == 0)
        {
          return {.drained = _drainWhenEmpty};
        }

        std::ranges::fill(output, static_cast<std::byte>(frames));
        return {.bytesWritten = output.size(), .positionFrames = frames};
      }

      void handlePositionAdvanced(std::uint32_t frames) noexcept override
      {
        ++_counts.positionCallbacks;
        publish({.positionFrames = frames});
      }

      void handleUnderrun() noexcept override { ++_counts.underruns; }

      void handleDrainComplete() noexcept override {}

      void handleRouteReady(std::string_view /*routeAnchor*/) noexcept override {}

      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override {}

      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}

      void handleBackendError(std::string_view message) noexcept override
      {
        ++_counts.backendErrors;
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

      bool _drainWhenEmpty = false;
      CallbackCounts _counts;
      std::uint32_t _nextPositionFrames = 0;
      std::uint32_t _followingPositionFrames = 0;
      std::mutex _mutex;
      std::condition_variable _observationChanged;
      std::optional<PlaybackObservation> _optObservation;
    };

    class NoMixerFactory final : public detail::AlsaMixerOpenFactory
    {
    public:
      std::unique_ptr<detail::AlsaMixerIo> open(::snd_pcm_t* /*pcm*/) override { return nullptr; }
    };

    struct PlaybackFixture final
    {
      RecordingRenderTarget target;
      NoMixerFactory mixerFactory;
      Device device{.id = DeviceId{kPcmName}, .displayName = "Private ALSA null PCM", .backendId = kBackendAlsa};
      AlsaExclusiveBackend backend{device,
                                   kProfileExclusive,
                                   detail::AlsaGraphPublisher{},
                                   std::make_unique<detail::AlsaMixerSession>(mixerFactory)};

      void requireOpen()
      {
        auto const res = backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16}, target);
        INFO((res ? "opened" : res.error().error.message));
        REQUIRE(res);
      }
    };

    constexpr std::uint32_t kRecoveryRetryGuard = 128;
    constexpr std::uint32_t kPreparedQueryGuard = 32;

    enum class TransientControl : std::uint8_t
    {
      None,
      Availability,
      Start
    };

    struct PauseHandshake final
    {
      std::binary_semaphore readyToPause{0};
      std::binary_semaphore pauseRequested{0};
      std::binary_semaphore pauseObserved{0};
      std::optional<std::uint32_t> optAvailCallsAtPause;
    };

    struct RunningFailure final
    {
      // Without a pause handshake, fail wait on the first short running read.
      PauseHandshake* pause = nullptr;
      bool shortRead = false;
      std::optional<std::uint32_t> optAvailCallsAtFailure;
    };

    struct TransferProbe final
    {
      std::int32_t commitError = 0;
      TransientControl interrupt = TransientControl::None;
      std::int32_t controlError = -EAGAIN;
      RecordingRenderTarget const* target = nullptr;
      std::uint32_t nonemptyCommits = 0;
      std::uint32_t acceptedCommits = 0;
      std::uint32_t preparedQueries = 0;
      std::uint32_t startCalls = 0;
      std::uint32_t successfulStarts = 0;
      std::uint32_t renderCallsAtStart = 0;
    };

    struct PcmFault final
    {
      std::int32_t availError = -ESTRPIPE;
      bool resumeInitiallyBusy = false;
      std::int32_t resumeResult = -ENOSYS;
      bool prepareInitiallyTransient = false;
      std::int32_t prepareError = 0;
      std::uint32_t availCalls = 0;
      std::uint32_t resumeCalls = 0;
      std::uint32_t prepareCalls = 0;
      std::uint32_t nativePrepareCalls = 0;
      TransferProbe transfer;
      PauseHandshake* pauseAtCommit = nullptr;
      std::optional<::snd_pcm_state_t> optShortReadState;
      RunningFailure* runningFailure = nullptr;
      std::promise<void> exited;
      std::binary_semaphore retrying{0};
      std::thread::id const owner = std::this_thread::get_id();
    };

    struct FaultRegistration final
    {
      PcmFault* script = nullptr;
      std::atomic<::snd_pcm_t*> pcm{nullptr};
    };

    FaultRegistration& registration()
    {
      static constinit auto active = FaultRegistration{};
      return active;
    }

    auto arm(PcmFault& fault)
    {
      registration().script = &fault;
      return utility::ScopedRegistration{[]
                                         {
                                           registration().pcm.store(nullptr);
                                           registration().script = nullptr;
                                         }};
    }

    PcmFault* workerFault(::snd_pcm_t* pcm)
    {
      // Only this open handle, and not the owner's drop/prepare after join.
      auto& active = registration();
      return active.pcm.load() == pcm && active.script->owner != std::this_thread::get_id() ? active.script : nullptr;
    }

    void observePreparedPause(::snd_pcm_t* pcm)
    {
      // The owner has requested pause, and the worker is applying it before
      // another availability check. An ordinary readiness probe cannot wake us.
      if (auto* const fault = workerFault(pcm); fault != nullptr && fault->pauseAtCommit != nullptr &&
                                                fault->pauseAtCommit->optAvailCallsAtPause == fault->availCalls)
      {
        fault->pauseAtCommit->optAvailCallsAtPause.reset();
        fault->pauseAtCommit->pauseObserved.release();
      }
    }

    void requireExplicitPcmStart()
    {
      // Keep the real PCM PREPARED until the worker starts it explicitly.
      // The owner can configure the private handle here, before start().
      auto* pcm = registration().pcm.load();
      REQUIRE(pcm != nullptr);
      ::snd_pcm_sw_params_t* params = nullptr;
      snd_pcm_sw_params_alloca(&params);
      ::snd_pcm_uframes_t boundary = 0;
      REQUIRE(::snd_pcm_sw_params_current(pcm, params) == 0);
      REQUIRE(::snd_pcm_sw_params_get_boundary(params, &boundary) == 0);
      REQUIRE(::snd_pcm_sw_params_set_start_threshold(pcm, params, boundary) == 0);
      REQUIRE(::snd_pcm_sw_params(pcm, params) == 0);
    }

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
    ::snd_pcm_sframes_t nativePcmAvailUpdate(::snd_pcm_t* pcm) asm("__real_snd_pcm_avail_update");
    ::snd_pcm_sframes_t testPcmAvailUpdate(::snd_pcm_t* pcm) asm("__wrap_snd_pcm_avail_update");
    int nativePcmResume(::snd_pcm_t* pcm) asm("__real_snd_pcm_resume");
    int testPcmResume(::snd_pcm_t* pcm) asm("__wrap_snd_pcm_resume");
    int nativePcmPrepare(::snd_pcm_t* pcm) asm("__real_snd_pcm_prepare");
    int testPcmPrepare(::snd_pcm_t* pcm) asm("__wrap_snd_pcm_prepare");
    ::snd_pcm_sframes_t nativePcmCommit(::snd_pcm_t* pcm,
                                        ::snd_pcm_uframes_t offset,
                                        ::snd_pcm_uframes_t frames) asm("__real_snd_pcm_mmap_commit");
    ::snd_pcm_sframes_t testPcmCommit(::snd_pcm_t* pcm,
                                      ::snd_pcm_uframes_t offset,
                                      ::snd_pcm_uframes_t frames) asm("__wrap_snd_pcm_mmap_commit");
    int nativePcmStart(::snd_pcm_t* pcm) asm("__real_snd_pcm_start");
    int testPcmStart(::snd_pcm_t* pcm) asm("__wrap_snd_pcm_start");
    int nativePcmPause(::snd_pcm_t* pcm, int enable) asm("__real_snd_pcm_pause");
    int testPcmPause(::snd_pcm_t* pcm, int enable) asm("__wrap_snd_pcm_pause");
    ::snd_pcm_state_t nativePcmState(::snd_pcm_t* pcm) asm("__real_snd_pcm_state");
    ::snd_pcm_state_t testPcmState(::snd_pcm_t* pcm) asm("__wrap_snd_pcm_state");
    int nativePcmWait(::snd_pcm_t* pcm, int timeout) asm("__real_snd_pcm_wait");
    int testPcmWait(::snd_pcm_t* pcm, int timeout) asm("__wrap_snd_pcm_wait");

    int testPcmWait(::snd_pcm_t* pcm, int timeout)
    {
      if (auto* const fault = workerFault(pcm); fault != nullptr && fault->runningFailure != nullptr &&
                                                fault->runningFailure->shortRead &&
                                                !fault->runningFailure->optAvailCallsAtFailure)
      {
        fault->runningFailure->optAvailCallsAtFailure = fault->availCalls;
        return -EIO;
      }

      return nativePcmWait(pcm, timeout);
    }

    ::snd_pcm_state_t testPcmState(::snd_pcm_t* pcm)
    {
      // Only the worker query following the one short read sees the scripted
      // state. Neither ALSA itself nor a recovery operation changes state here.
      if (auto* const fault = workerFault(pcm); fault != nullptr && fault->availCalls == 1 && fault->optShortReadState)
      {
        return *std::exchange(fault->optShortReadState, std::nullopt);
      }

      auto const state = nativePcmState(pcm);

      if (state == SND_PCM_STATE_PREPARED)
      {
        observePreparedPause(pcm);
      }

      return state;
    }

    int testPcmPause(::snd_pcm_t* pcm, int enable)
    {
      auto* const fault = workerFault(pcm);
      auto* const running = fault != nullptr ? fault->runningFailure : nullptr;

      if (running != nullptr && running->pause != nullptr && enable == 0 && nativePcmState(pcm) == SND_PCM_STATE_PAUSED)
      {
        running->optAvailCallsAtFailure = fault->availCalls;
        return -EIO;
      }

      auto const status = nativePcmPause(pcm, enable);

      if (running != nullptr && running->pause != nullptr && enable != 0 && status == 0 &&
          nativePcmState(pcm) == SND_PCM_STATE_PAUSED)
      {
        running->pause->pauseObserved.release();
      }

      // Also acknowledge a worker that attempted an invalid hardware pause.
      // Return the real error; the fallback prepare must be observable.
      if (enable != 0 && status == -EBADFD)
      {
        observePreparedPause(pcm);
      }

      return status;
    }

    ::snd_pcm_sframes_t testPcmAvailUpdate(::snd_pcm_t* pcm)
    {
      if (auto* const fault = workerFault(pcm); fault != nullptr)
      {
        if (++fault->availCalls == 1)
        {
          fault->exited.set_value_at_thread_exit();

          if (fault->optShortReadState)
          {
            return 1;
          }

          if (fault->availError != 0)
          {
            return fault->availError;
          }
        }

        if (auto* const running = fault->runningFailure; running != nullptr)
        {
          // Let an ignored failure read native availability once, but bound
          // retries if the real paused PCM cannot make further progress.
          if (running->optAvailCallsAtFailure && fault->availCalls > *running->optAvailCallsAtFailure + 1U)
          {
            return -ENODEV;
          }

          if (running->pause == nullptr && !running->shortRead && fault->transfer.successfulStarts == 1 &&
              nativePcmState(pcm) == SND_PCM_STATE_RUNNING)
          {
            running->shortRead = true;
            return 1;
          }
        }

        if (auto& transfer = fault->transfer; transfer.target != nullptr && transfer.acceptedCommits == 1 &&
                                              fault->prepareCalls == 0 && nativePcmState(pcm) == SND_PCM_STATE_PREPARED)
        {
          if (++transfer.preparedQueries == 1 && transfer.interrupt == TransientControl::Availability)
          {
            return transfer.controlError;
          }

          // Null has no hardware backlog. Keep reporting the retained full
          // buffer while PREPARED; let a worker that never starts it drain
          // naturally after a bounded number of probes.
          if (transfer.preparedQueries < kPreparedQueryGuard)
          {
            return 0;
          }
        }
      }

      return nativePcmAvailUpdate(pcm);
    }

    ::snd_pcm_sframes_t testPcmCommit(::snd_pcm_t* pcm, ::snd_pcm_uframes_t offset, ::snd_pcm_uframes_t frames)
    {
      auto* const fault = workerFault(pcm);

      if (fault == nullptr || frames == 0)
      {
        return nativePcmCommit(pcm, offset, frames);
      }

      auto& transfer = fault->transfer;

      if (++transfer.nonemptyCommits == 1 && transfer.commitError != 0)
      {
        // Release the actual mapping, accepting none of the rendered frames.
        std::ignore = nativePcmCommit(pcm, offset, 0);
        return transfer.commitError;
      }

      auto const committed = nativePcmCommit(pcm, offset, frames);

      if (committed > 0)
      {
        ++transfer.acceptedCommits;

        if (auto* const pause = fault->pauseAtCommit;
            pause != nullptr && transfer.acceptedCommits == 1 && nativePcmState(pcm) == SND_PCM_STATE_PREPARED)
        {
          pause->readyToPause.release();

          if (pause->pauseRequested.try_acquire_for(kCallbackTimeout))
          {
            pause->optAvailCallsAtPause = fault->availCalls;
          }
        }
      }

      return committed;
    }

    int testPcmStart(::snd_pcm_t* pcm)
    {
      auto* const fault = workerFault(pcm);

      if (fault == nullptr || fault->transfer.target == nullptr)
      {
        return nativePcmStart(pcm);
      }

      auto& transfer = fault->transfer;

      if (++transfer.startCalls == 1 && transfer.interrupt == TransientControl::Start)
      {
        return transfer.controlError;
      }

      auto const status = nativePcmStart(pcm);

      if (status == 0)
      {
        ++transfer.successfulStarts;
        transfer.renderCallsAtStart = transfer.target->counts().renderCalls;

        if (auto* const running = fault->runningFailure;
            running != nullptr && running->pause != nullptr && transfer.successfulStarts == 1)
        {
          running->pause->readyToPause.release();
          std::ignore = running->pause->pauseRequested.try_acquire_for(kCallbackTimeout);
        }
      }

      return status;
    }

    int testPcmResume(::snd_pcm_t* pcm)
    {
      if (auto* const fault = workerFault(pcm); fault != nullptr)
      {
        ++fault->resumeCalls;

        if (fault->resumeInitiallyBusy && fault->resumeCalls == 1)
        {
          return -EAGAIN;
        }

        if (fault->resumeResult == -EAGAIN && fault->resumeCalls == 2)
        {
          fault->retrying.release();
        }

        // A worker that ignores stop must still be joinable in the failing test.
        return fault->resumeCalls >= kRecoveryRetryGuard ? -ENOSYS : fault->resumeResult;
      }

      return nativePcmResume(pcm);
    }

    int testPcmPrepare(::snd_pcm_t* pcm)
    {
      if (auto* const fault = workerFault(pcm); fault != nullptr)
      {
        ++fault->prepareCalls;

        if (fault->prepareInitiallyTransient && fault->prepareCalls <= 2)
        {
          return fault->prepareCalls == 1 ? -EAGAIN : -EINTR;
        }

        if (fault->prepareError == -EAGAIN)
        {
          if (fault->prepareCalls == 2)
          {
            fault->retrying.release();
          }

          // Let even a worker that ignores cancellation leave the retry loop.
          if (fault->prepareCalls < kRecoveryRetryGuard)
          {
            return -EAGAIN;
          }
        }
        else if (fault->prepareError != 0)
        {
          return fault->prepareError;
        }

        ++fault->nativePrepareCalls;
      }

      return nativePcmPrepare(pcm);
    }

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

      if (status == 0 && registration().script != nullptr)
      {
        registration().pcm.store(*pcm);
      }

      return status;
    }

    ::snd_pcm_type_t testPcmType(::snd_pcm_t* pcm)
    {
      return std::string_view{::snd_pcm_name(pcm)} == kPcmName ? SND_PCM_TYPE_HW : nativePcmType(pcm);
    }
  }

  TEST_CASE("AlsaExclusiveBackend - start advances playback after retiring a run with pause requested", "[concurrency]")
  {
    auto playback = PlaybackFixture{};
    auto& target = playback.target;
    auto& backend = playback.backend;
    playback.requireOpen();

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
      playback.requireOpen();
      requireRestartProgress();
    }
  }

  TEST_CASE("AlsaExclusiveBackend - PCM recovery advances playback after a recoverable availability error",
            "[concurrency]")
  {
    auto fault = PcmFault{};
    std::uint32_t expectedResumes = 0;
    std::uint32_t expectedPrepares = 0;
    std::uint32_t expectedUnderruns = 0;

    SECTION("suspend without resume support falls back to prepare")
    {
      expectedResumes = 1;
      expectedPrepares = 1;
    }

    SECTION("suspend resumes successfully after one busy response")
    {
      fault.resumeInitiallyBusy = true;
      fault.resumeResult = 0;
      expectedResumes = 2;
    }

    SECTION("availability temporarily busy")
    {
      fault.availError = -EAGAIN;
    }

    SECTION("availability interrupted")
    {
      fault.availError = -EINTR;
    }

    SECTION("underrun is reported and prepared once")
    {
      fault.availError = -EPIPE;
      expectedPrepares = 1;
      expectedUnderruns = 1;
    }

    SECTION("underrun prepare retries busy and interrupted calls before succeeding")
    {
      fault.availError = -EPIPE;
      fault.prepareInitiallyTransient = true;
      expectedPrepares = 3;
      expectedUnderruns = 1;
    }

    auto guard = arm(fault);
    auto playback = PlaybackFixture{};
    playback.requireOpen();
    playback.target.supplyPositionFrames(13);
    playback.backend.start();
    requirePositionAdvanced(playback.target, 13);
    playback.backend.stop();
    CHECK(fault.availCalls >= 2);
    CHECK(fault.resumeCalls == expectedResumes);
    CHECK(fault.prepareCalls == expectedPrepares);
    CHECK(fault.nativePrepareCalls == (expectedPrepares != 0 ? 1U : 0U));
    CHECK(playback.target.counts().underruns == expectedUnderruns);
    CHECK(playback.target.counts().backendErrors == 0);
  }

  TEST_CASE("AlsaExclusiveBackend - PCM recovery reports a terminal error once and retires the worker", "[concurrency]")
  {
    auto fault = PcmFault{};

    SECTION("suspend fallback prepare failed")
    {
      fault.prepareError = -EIO;
    }

    SECTION("underrun prepare failed")
    {
      fault.availError = -EPIPE;
      fault.prepareError = -EIO;
    }

    SECTION("I/O error")
    {
      fault.availError = -EIO;
    }

    SECTION("file descriptor in bad state")
    {
      fault.availError = -EBADFD;
    }

    SECTION("device gone")
    {
      fault.availError = -ENODEV;
    }

    SECTION("invalid file descriptor")
    {
      fault.availError = -EBADF;
    }

    auto exited = fault.exited.get_future();
    auto guard = arm(fault);
    auto playback = PlaybackFixture{};
    playback.requireOpen();
    // Let a buggy worker drain real PCM on a second attempt instead of busy-looping.
    playback.target.drainWhenEmpty();
    playback.backend.start();
    REQUIRE(exited.wait_for(kCallbackTimeout) == std::future_status::ready);
    // The wait proves natural retirement; join before reading worker-owned counters.
    playback.backend.stop();
    CHECK(fault.availCalls == 1);
    CHECK(fault.resumeCalls == (fault.availError == -ESTRPIPE ? 1U : 0U));
    CHECK(fault.prepareCalls == (fault.prepareError != 0 ? 1U : 0U));
    CHECK(playback.target.counts().underruns == (fault.availError == -EPIPE ? 1U : 0U));
    REQUIRE(playback.target.counts().backendErrors == 1);
    auto const optObservation = playback.target.waitForObservation();
    REQUIRE(optObservation);
    REQUIRE(optObservation->optBackendError);
    CHECK_FALSE(optObservation->optBackendError->empty());
  }

  TEST_CASE("AlsaExclusiveBackend - PCM recovery stops while resume keeps returning EAGAIN", "[concurrency]")
  {
    auto fault = PcmFault{};
    fault.resumeResult = -EAGAIN;
    auto guard = arm(fault);
    auto playback = PlaybackFixture{};
    playback.requireOpen();
    playback.backend.start();
    REQUIRE(fault.retrying.try_acquire_for(kCallbackTimeout));
    playback.backend.stop();
    CHECK(fault.resumeCalls < kRecoveryRetryGuard);
    CHECK(fault.prepareCalls == 0);
    CHECK(playback.target.counts().backendErrors == 0);
  }

  TEST_CASE("AlsaExclusiveBackend - PCM transfer rejection terminates without crediting rendered input",
            "[concurrency]")
  {
    auto fault = PcmFault{};
    fault.availError = 0;

    SECTION("I/O error")
    {
      fault.transfer.commitError = -EIO;
    }

    SECTION("interrupted commit")
    {
      fault.transfer.commitError = -EINTR;
    }

    SECTION("temporarily busy commit")
    {
      fault.transfer.commitError = -EAGAIN;
    }

    auto guard = arm(fault);
    auto playback = PlaybackFixture{};
    playback.requireOpen();
    playback.target.supplyPositionFrames(13);
    playback.target.drainWhenEmpty();
    auto exited = fault.exited.get_future();
    playback.backend.start();
    REQUIRE(exited.wait_for(kCallbackTimeout) == std::future_status::ready);
    playback.backend.stop();
    CHECK(playback.target.counts().renderCalls == 1);
    CHECK(fault.transfer.nonemptyCommits == 1);
    CHECK(fault.transfer.acceptedCommits == 0);
    CHECK(playback.target.counts().positionCallbacks == 0);
    CHECK(playback.target.counts().backendErrors == 1);
  }

  TEST_CASE("AlsaExclusiveBackend - PCM transfer retries prepared controls without consuming more input",
            "[concurrency]")
  {
    auto fault = PcmFault{};
    fault.availError = 0;

    SECTION("availability temporarily busy")
    {
      fault.transfer.interrupt = TransientControl::Availability;
    }

    SECTION("start interrupted")
    {
      fault.transfer.interrupt = TransientControl::Start;
      fault.transfer.controlError = -EINTR;
    }

    SECTION("availability suspended with retained frames")
    {
      fault.transfer.interrupt = TransientControl::Availability;
      fault.transfer.controlError = -ESTRPIPE;
    }

    SECTION("start suspended with retained frames")
    {
      fault.transfer.interrupt = TransientControl::Start;
      fault.transfer.controlError = -ESTRPIPE;
    }

    // The private PCM stays PREPARED. Inject successful resume without a
    // kernel suspend, preserving the real commit and its single position credit.
    fault.resumeResult = 0;
    auto guard = arm(fault);
    auto playback = PlaybackFixture{};
    playback.requireOpen();
    requireExplicitPcmStart();
    fault.transfer.target = &playback.target;
    playback.target.supplyPositionFrames(13);
    playback.target.drainWhenEmpty();
    auto exited = fault.exited.get_future();
    playback.backend.start();
    REQUIRE(exited.wait_for(kCallbackTimeout) == std::future_status::ready);
    playback.backend.stop();
    auto const interruptedQuery = fault.transfer.interrupt == TransientControl::Availability;
    CHECK(fault.transfer.preparedQueries >= (interruptedQuery ? 2U : 1U));
    CHECK(fault.transfer.startCalls == (interruptedQuery ? 1U : 2U));
    CHECK(fault.transfer.successfulStarts == 1);
    CHECK(fault.transfer.renderCallsAtStart == 1);
    CHECK(fault.resumeCalls == (fault.transfer.controlError == -ESTRPIPE ? 1U : 0U));
    CHECK(fault.prepareCalls == 0);
    CHECK(fault.transfer.nonemptyCommits == 1);
    CHECK(fault.transfer.acceptedCommits == 1);
    CHECK(playback.target.counts().backendErrors == 0);
    REQUIRE(playback.target.counts().positionCallbacks == 1);
    requirePositionAdvanced(playback.target, 13);
  }

  TEST_CASE("AlsaExclusiveBackend - pause and resume retain a prepared commit before the first start", "[concurrency]")
  {
    auto pause = PauseHandshake{};
    auto fault = PcmFault{};
    fault.availError = 0;
    fault.pauseAtCommit = &pause;
    auto guard = arm(fault);
    auto playback = PlaybackFixture{};
    playback.requireOpen();
    requireExplicitPcmStart();
    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    REQUIRE(::snd_pcm_hw_params_current(registration().pcm.load(), params) == 0);
    REQUIRE(::snd_pcm_hw_params_can_pause(params) == 1);
    fault.transfer.target = &playback.target;
    playback.target.supplyPositionFrames(13);
    playback.target.drainWhenEmpty();
    auto exited = fault.exited.get_future();
    playback.backend.start();
    REQUIRE(pause.readyToPause.try_acquire_for(kCallbackTimeout));
    playback.backend.pause();
    pause.pauseRequested.release();
    REQUIRE(pause.pauseObserved.try_acquire_for(kCallbackTimeout));
    playback.backend.resume();
    REQUIRE(exited.wait_for(kCallbackTimeout) == std::future_status::ready);
    playback.backend.stop();
    CHECK(fault.prepareCalls == 0);
    CHECK(fault.transfer.nonemptyCommits == 1);
    CHECK(fault.transfer.acceptedCommits == 1);
    CHECK(fault.transfer.successfulStarts == 1);
    CHECK(fault.transfer.renderCallsAtStart == 1);
    CHECK(playback.target.counts().backendErrors == 0);
    REQUIRE(playback.target.counts().positionCallbacks == 1);
    requirePositionAdvanced(playback.target, 13);
  }

  TEST_CASE("AlsaExclusiveBackend - PCM readiness handles an error state with short positive availability",
            "[concurrency]")
  {
    auto fault = PcmFault{};
    fault.availError = 0;
    fault.resumeResult = 0;
    auto state = SND_PCM_STATE_XRUN;

    SECTION("underrun")
    {
      state = SND_PCM_STATE_XRUN;
    }

    SECTION("suspended")
    {
      state = SND_PCM_STATE_SUSPENDED;
    }

    SECTION("disconnected")
    {
      state = SND_PCM_STATE_DISCONNECTED;
    }

    fault.optShortReadState = state;
    auto guard = arm(fault);
    auto playback = PlaybackFixture{};
    playback.requireOpen();
    playback.target.supplyPositionFrames(13);
    playback.target.drainWhenEmpty();
    auto exited = fault.exited.get_future();
    playback.backend.start();
    REQUIRE(exited.wait_for(kCallbackTimeout) == std::future_status::ready);
    playback.backend.stop();
    CHECK_FALSE(fault.optShortReadState);
    CHECK(fault.prepareCalls == (state == SND_PCM_STATE_XRUN ? 1U : 0U));
    CHECK(playback.target.counts().underruns == (state == SND_PCM_STATE_XRUN ? 1U : 0U));
    CHECK(fault.resumeCalls == (state == SND_PCM_STATE_SUSPENDED ? 1U : 0U));

    if (state == SND_PCM_STATE_DISCONNECTED)
    {
      CHECK(fault.availCalls == 1);
      CHECK(playback.target.counts().renderCalls == 0);
      CHECK(playback.target.counts().backendErrors == 1);
    }
    else
    {
      CHECK(playback.target.counts().backendErrors == 0);
      requirePositionAdvanced(playback.target, 13);
    }
  }

  TEST_CASE("AlsaExclusiveBackend - PCM readiness retires a failed or cancelled control without more input",
            "[concurrency]")
  {
    auto pause = PauseHandshake{};
    auto running = RunningFailure{};
    auto fault = PcmFault{};
    fault.availError = 0;
    fault.runningFailure = &running;

    SECTION("running PCM wait fails after short availability")
    {
      running.pause = nullptr;
    }

    SECTION("native unpause and fallback prepare both fail")
    {
      running.pause = &pause;
      fault.prepareError = -EIO;
    }

    SECTION("stop cancels fallback prepare while it keeps returning EAGAIN")
    {
      running.pause = &pause;
      fault.prepareError = -EAGAIN;
    }

    auto guard = arm(fault);
    auto playback = PlaybackFixture{};
    playback.requireOpen();
    requireExplicitPcmStart();
    fault.transfer.target = &playback.target;
    playback.target.supplyPositionFrames(13);
    auto exited = fault.exited.get_future();
    playback.backend.start();

    if (running.pause != nullptr)
    {
      REQUIRE(pause.readyToPause.try_acquire_for(kCallbackTimeout));
      playback.backend.pause();
      pause.pauseRequested.release();
      REQUIRE(pause.pauseObserved.try_acquire_for(kCallbackTimeout));
      playback.backend.resume();
    }

    bool const cancelPrepare = fault.prepareError == -EAGAIN;

    if (cancelPrepare)
    {
      REQUIRE(fault.retrying.try_acquire_for(kCallbackTimeout));
    }
    else
    {
      REQUIRE(exited.wait_for(kCallbackTimeout) == std::future_status::ready);
    }

    playback.backend.stop();
    REQUIRE(running.optAvailCallsAtFailure);
    CHECK(fault.availCalls == *running.optAvailCallsAtFailure);

    if (cancelPrepare)
    {
      CHECK(fault.prepareCalls >= 2);
      CHECK(fault.prepareCalls < kRecoveryRetryGuard);
    }
    else
    {
      CHECK(fault.prepareCalls == (running.pause != nullptr ? 1U : 0U));
    }

    CHECK(fault.nativePrepareCalls == 0);
    CHECK(fault.transfer.successfulStarts == 1);
    CHECK(playback.target.counts().renderCalls == 1);
    CHECK(playback.target.counts().backendErrors == (cancelPrepare ? 0U : 1U));
    REQUIRE(playback.target.counts().positionCallbacks == 1);
    requirePositionAdvanced(playback.target, 13);
  }

  TEST_CASE("AlsaExclusiveBackend - PCM transfer recovers from a rejected commit and credits only later input",
            "[concurrency]")
  {
    auto fault = PcmFault{};
    fault.availError = 0;
    fault.transfer.commitError = -ESTRPIPE;
    std::uint32_t expectedUnderruns = 0;
    std::uint32_t expectedResumes = 1;
    std::uint32_t expectedPrepares = 0;

    SECTION("suspended commit resumes successfully")
    {
      fault.resumeResult = 0;
    }

    SECTION("underrun on commit requires prepare")
    {
      fault.transfer.commitError = -EPIPE;
      expectedUnderruns = 1;
      expectedResumes = 0;
      expectedPrepares = 1;
    }

    SECTION("suspended commit falls back to prepare when resume is unsupported")
    {
      fault.resumeResult = -ENOSYS;
      expectedPrepares = 1;
    }

    auto guard = arm(fault);
    auto playback = PlaybackFixture{};
    playback.requireOpen();
    playback.target.supplyPositionFrames(13, 29);
    playback.target.drainWhenEmpty();
    auto exited = fault.exited.get_future();
    playback.backend.start();
    REQUIRE(exited.wait_for(kCallbackTimeout) == std::future_status::ready);
    playback.backend.stop();
    CHECK(playback.target.counts().underruns == expectedUnderruns);
    CHECK(fault.resumeCalls == expectedResumes);
    CHECK(fault.prepareCalls == expectedPrepares);
    CHECK(fault.nativePrepareCalls == expectedPrepares);
    CHECK(fault.transfer.nonemptyCommits == 2);
    CHECK(fault.transfer.acceptedCommits == 1);
    CHECK(playback.target.counts().backendErrors == 0);
    REQUIRE(playback.target.counts().positionCallbacks == 1);
    requirePositionAdvanced(playback.target, 29);
  }
} // namespace ao::audio::backend::test

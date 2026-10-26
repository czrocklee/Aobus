// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/WasapiSharedBackend.h"

#include "lib/audio/backend/detail/WasapiError.h"
#include "lib/audio/backend/detail/WasapiFormat.h"
#include "lib/audio/backend/detail/WasapiFormatObservation.h"
#include "lib/audio/backend/detail/WasapiGraphRegistry.h"
#include "lib/audio/backend/detail/WasapiRenderBuffer.h"
#include "test/audio/BackendTestSupport.h"
#include "test/audio/backend/WasapiFormatTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace ao::audio::backend::test
{
  namespace
  {
    Device testDevice()
    {
      return {.id = DeviceId{"endpoint-a"}, .displayName = "Test endpoint", .backendId = kBackendWasapi};
    }
  } // namespace

  TEST_CASE("WasapiSharedBackend - identity and unopened lifecycle are stable", "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    CHECK(backend.backendId() == kBackendWasapi);
    CHECK(backend.profileId() == kProfileShared);

    backend.start();
    backend.pause();
    backend.resume();
    backend.flush();
    backend.stop();
    backend.close();
  }

  TEST_CASE("WasapiSharedBackend - cached properties round trip before opening a device",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    REQUIRE(backend.set(props::kVolume, 0.25F));
    REQUIRE(backend.set(props::kMuted, true));
    auto const volumeRes = backend.get(props::kVolume);
    auto const mutedRes = backend.get(props::kMuted);

    REQUIRE(volumeRes);
    REQUIRE(mutedRes);
    CHECK(*volumeRes == 0.25F);
    CHECK(*mutedRes);

    auto const volumeInfo = backend.queryProperty(PropertyId::Volume);
    auto const mutedInfo = backend.queryProperty(PropertyId::Muted);
    CHECK(volumeInfo.canRead);
    CHECK(volumeInfo.canWrite);
    CHECK_FALSE(volumeInfo.isAvailable);
    CHECK_FALSE(volumeInfo.emitsChangeNotifications);
    CHECK_FALSE(volumeInfo.isHardwareAssisted);
    CHECK(mutedInfo == volumeInfo);
  }

  TEST_CASE("WasapiSharedBackend - unopened property intent does not claim applied session state",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto received = std::vector<flow::Graph>{};
    auto sub = registryPtr->subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    REQUIRE(backend.set(props::kVolume, -0.5F));
    auto volumeRes = backend.get(props::kVolume);
    REQUIRE(volumeRes);
    CHECK(*volumeRes == 0.0F);
    REQUIRE(received.size() == 2);
    REQUIRE(received.back().nodes.size() == 2);
    CHECK_FALSE(received.back().nodes[1].softwareVolumeNotUnity);
    CHECK(received.back().nodes[1].maxSoftwareGain == 1.0F);

    auto const unappliedGraph = received.back();
    CHECK(received.front() == unappliedGraph);
    REQUIRE(backend.set(props::kVolume, 1.5F));
    volumeRes = backend.get(props::kVolume);
    REQUIRE(volumeRes);
    CHECK(*volumeRes == 1.0F);
    // Changed unopened intent leaves the applied-session graph unchanged, so it is not redelivered.
    REQUIRE(received.size() == 2);
    CHECK(received.back() == unappliedGraph);
    CHECK_FALSE(received.back().nodes[1].softwareVolumeNotUnity);
    CHECK(received.back().nodes[1].maxSoftwareGain == 1.0F);

    backend.close();
    REQUIRE(received.size() == 3);
    CHECK(received.back().nodes.empty());
    CHECK(received.back().connections.empty());
  }

  TEST_CASE("WasapiSharedBackend - a partial speaker set is rejected before endpoint access",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    // The endpoint id names no real endpoint, so a regressed guard could only
    // surface an endpoint lookup failure here, never a native handle.
    // One assigned role cannot describe this two-channel source.
    auto const openedRes =
      backend.open(SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x1}, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code == Error::Code::NotSupported);
    CHECK(openedRes.error().error.message == "WASAPI requires a complete supported speaker set");
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
  }

  TEST_CASE(
    "WasapiSharedBackend - an unrepresentable source is refused as a track incompatibility before endpoint access",
    "[audio][unit][wasapi][backend]")
  {
    // The endpoint id names no real endpoint, so a regressed guard could only
    // surface an endpoint lookup failure here, never a native handle.
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    SECTION("a 33-bit integer signal has no lossless encoding at all")
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 48000,
                                             .channels = 2,
                                             .precisionBits = 33,
                                             .sampleKind = SampleKind::Integer,
                                             .speakerMask = 0x30};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(openedRes.error().error.message == "No lossless WASAPI input encoding is available");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }

    SECTION("a non-32-bit float signal has no lossless encoding at all")
    {
      auto const openedRes = backend.open(
        SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::FloatingPoint},
        target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(openedRes.error().error.message == "No lossless WASAPI input encoding is available");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    }

    SECTION("an unsupported precision with a partial mask still reports the precision refusal")
    {
      // A valid geometry and an inadmissible partial set: the lossless
      // encoding refusal owns the diagnostic, so the layout refusal cannot
      // take over for a caller invalid in both dimensions.
      auto const sourceFormat = SignalFormat{
        .sampleRate = 48000, .channels = 2, .precisionBits = 33, .sampleKind = SampleKind::Integer, .speakerMask = 0x1};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(openedRes.error().error.message == "No lossless WASAPI input encoding is available");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }
  }

  TEST_CASE("WasapiSharedBackend - zero geometry is refused as invalid input before endpoint access",
            "[audio][unit][wasapi][backend]")
  {
    // The endpoint id names no real endpoint, so a regressed guard could only
    // surface an endpoint lookup failure here, never a native handle.
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    SECTION("a zero sample rate is invalid geometry")
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 0, .channels = 2, .precisionBits = 16};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "WASAPI PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }

    SECTION("a zero channel count is invalid geometry")
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 0, .precisionBits = 16};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "WASAPI PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }
  }

  TEST_CASE("WasapiSharedBackend - geometry refusal outranks layout and precision refusals",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};
    auto target = ::ao::audio::test::NoopRenderTarget{};

    SECTION("a partial speaker set with no sample rate still reports the geometry refusal")
    {
      auto const openedRes =
        backend.open(SignalFormat{.sampleRate = 0, .channels = 2, .precisionBits = 16, .speakerMask = 0x1}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "WASAPI PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }

    SECTION("an unrepresentable source with no channels still reports the geometry refusal")
    {
      auto const openedRes =
        backend.open(SignalFormat{.sampleRate = 48000, .channels = 0, .precisionBits = 33}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "WASAPI PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }
  }

  TEST_CASE("WasapiSharedBackend - descriptor geometry refusal outranks layout refusal before endpoint access",
            "[audio][unit][wasapi][backend]")
  {
    // The fake endpoint cannot supply native evidence. Two-channel S16 needs
    // four bytes per frame, so this rate cannot fit the WAVE byte-rate field.
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};
    auto target = ::ao::audio::test::NoopRenderTarget{};
    auto sourceFormat = SignalFormat{.sampleRate = std::numeric_limits<std::uint32_t>::max(),
                                     .channels = 2,
                                     .precisionBits = 16,
                                     .sampleKind = SampleKind::Integer};

    SECTION("an unknown source speaker set cannot hide descriptor overflow")
    {
      sourceFormat.speakerMask = 0;
    }

    SECTION("a complete source speaker set cannot hide descriptor overflow")
    {
      sourceFormat.speakerMask = 0x3;
    }

    SECTION("a partial source speaker set cannot replace the geometry diagnostic")
    {
      sourceFormat.speakerMask = 0x1;
    }

    auto const openedRes = backend.open(sourceFormat, target);

    REQUIRE_FALSE(openedRes);
    CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
    CHECK(openedRes.error().error.message == "WASAPI PCM descriptor geometry is not representable");
    CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
  }

  TEST_CASE("WasapiSharedBackend - prewarm preserves complete admitted sets without endpoint negotiation",
            "[audio][unit][wasapi][backend]")
  {
    auto registryPtr = std::make_shared<detail::WasapiGraphRegistry>();
    auto backend = WasapiSharedBackend{testDevice(), kProfileShared, registryPtr};

    for (auto const source :
         {SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16},
          SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3},
          SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x30},
          SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x5},
          SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16, .speakerMask = 0x1},
          SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16, .speakerMask = 0x4},
          SignalFormat{.sampleRate = 48000, .channels = 4, .precisionBits = 16, .speakerMask = 0x33}})
    {
      auto const optHint = backend.prewarmFormatHint(source);
      REQUIRE(optHint);
      CHECK(optHint->sampleRate == 48000U);
      CHECK(optHint->channels == source.channels);
      CHECK(optHint->speakerMask == source.speakerMask);
      CHECK(optHint->encoding == SampleEncoding::Signed16Le);
    }

    CHECK_FALSE(backend.prewarmFormatHint(
      SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 1}));
  }

  TEST_CASE("prepareWasapiRenderPacket pads short PCM without advancing silence", "[audio][unit][wasapi][render]")
  {
    auto buffer = std::array<std::byte, 16>{};
    std::ranges::fill(buffer, std::byte{0x5a});

    auto const packet = detail::prepareWasapiRenderPacket(
      buffer, 4, RenderPcmResult{.bytesWritten = 8, .positionFrameOffset = 1, .positionFrames = 1, .drained = false});

    CHECK(packet.renderedFrames == 2);
    CHECK(packet.framesToRelease == 4);
    CHECK(packet.positionFrames == 1);
    CHECK(packet.underrun);
    CHECK_FALSE(packet.drained);

    for (std::size_t index = 0; index < buffer.size(); ++index)
    {
      CHECK(buffer[index] == (index < 8 ? std::byte{0x5a} : std::byte{0}));
    }
  }

  TEST_CASE("prepareWasapiRenderPacket releases drained short and empty packets correctly",
            "[audio][unit][wasapi][render]")
  {
    auto buffer = std::array<std::byte, 16>{};
    std::ranges::fill(buffer, std::byte{0x5a});

    SECTION("a final PCM prefix commits a complete silent-padded packet")
    {
      auto const packet =
        detail::prepareWasapiRenderPacket(buffer, 4, RenderPcmResult{.bytesWritten = 4, .drained = true});

      CHECK(packet.renderedFrames == 1);
      CHECK(packet.framesToRelease == 4);
      CHECK_FALSE(packet.underrun);
      CHECK(packet.drained);
      CHECK(buffer.front() == std::byte{0x5a});
      CHECK(buffer.back() == std::byte{0});
    }

    SECTION("an empty drained packet is released without a commit")
    {
      auto const packet =
        detail::prepareWasapiRenderPacket(buffer, 4, RenderPcmResult{.bytesWritten = 0, .drained = true});

      CHECK(packet.renderedFrames == 0);
      CHECK(packet.framesToRelease == 0);
      CHECK_FALSE(packet.underrun);
      CHECK(packet.drained);
      CHECK(buffer.back() == std::byte{0x5a});
    }
  }

  TEST_CASE("observeWaveFormat parses plain PCM tags without inventing speaker roles", "[audio][unit][wasapi][format]")
  {
    struct PlainWaveCase final
    {
      WORD tag;
      WORD containerBits;
      WORD stride;
      DWORD byteRate;
      SampleEncoding encoding;
    };
    auto const cases = std::array{PlainWaveCase{WAVE_FORMAT_PCM, 16, 4, 192000, SampleEncoding::Signed16Le},
                                  PlainWaveCase{WAVE_FORMAT_IEEE_FLOAT, 32, 8, 384000, SampleEncoding::Float32Le}};

    for (auto const& fixture : cases)
    {
      CAPTURE(fixture.tag, fixture.containerBits);
      auto wave = WAVEFORMATEX{};
      wave.wFormatTag = fixture.tag;
      wave.nChannels = 2;
      wave.nSamplesPerSec = 48000;
      wave.wBitsPerSample = fixture.containerBits;
      wave.nBlockAlign = fixture.stride;
      wave.nAvgBytesPerSec = fixture.byteRate;

      auto const observationRes = detail::observeWaveFormat(wave);
      REQUIRE(observationRes);
      CHECK(observationRes->sampleRate == 48000U);
      CHECK(observationRes->channels == 2U);
      CHECK(observationRes->speakerMask == 0U);
      REQUIRE(observationRes->optFormat);
      CHECK(observationRes->optFormat->sampleRate == 48000U);
      CHECK(observationRes->optFormat->channels == 2U);
      CHECK(observationRes->optFormat->encoding == fixture.encoding);
      CHECK(observationRes->optFormat->speakerMask == 0U);

      auto badStride = wave;
      badStride.nBlockAlign = static_cast<WORD>(fixture.stride - 1U);
      auto badByteRate = wave;
      badByteRate.nAvgBytesPerSec = fixture.byteRate - 1U;

      for (auto const& contradictory : std::array{badStride, badByteRate})
      {
        auto const contradictoryRes = detail::observeWaveFormat(contradictory);
        REQUIRE(contradictoryRes);
        CHECK(contradictoryRes->sampleRate == 48000U);
        CHECK(contradictoryRes->channels == 2U);
        CHECK(contradictoryRes->speakerMask == 0U);
        CHECK_FALSE(contradictoryRes->optFormat);
      }
    }
  }

  TEST_CASE("observeWaveFormat preserves endpoint mix precision", "[audio][unit][wasapi][format]")
  {
    auto wave = WAVEFORMATEXTENSIBLE{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = 2;
    wave.Format.nSamplesPerSec = 48000;
    wave.Format.wBitsPerSample = 32;
    wave.Format.nBlockAlign = 8;
    wave.Format.nAvgBytesPerSec = 384000;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
    wave.Samples.wValidBitsPerSample = 24; // NOLINT(cppcoreguidelines-pro-type-union-access)
    wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    auto const observationRes = detail::observeWaveFormat(wave.Format);

    REQUIRE(observationRes);
    REQUIRE(observationRes->optFormat);
    CHECK(observationRes->optFormat->sampleRate == 48000);
    CHECK(observationRes->optFormat->channels == 2);
    CHECK(observationRes->optFormat->encoding == SampleEncoding::Signed24In32Le);
  }

  TEST_CASE("WasapiFormat - accepted descriptors preserve declared role masks at all channel counts",
            "[audio][unit][wasapi][format]")
  {
    struct DescriptorCase final
    {
      std::uint8_t channels;
      std::uint32_t sourceMask;
      std::uint32_t nativeMask;
      bool isMapped;
    };
    // Known layouts keep their literal WAVE masks in the Initialize
    // descriptor: quad 0x33, 5.1 back 0x3F, 5.1 side 0x60F and 7.1 0x63F.
    // Only a zero-mask source keeps the legacy defaults: FC/FL-FR fill for
    // one or two channels, and direct-out (mask zero) above two.
    auto const cases = std::array{DescriptorCase{1, 0x1, 0x1, true},
                                  DescriptorCase{1, 0x4, 0x4, true},
                                  DescriptorCase{2, 0x3, 0x3, true},
                                  DescriptorCase{2, 0x30, 0x30, true},
                                  DescriptorCase{2, 0x5, 0x5, true},
                                  DescriptorCase{1, 0, 0x4, false},
                                  DescriptorCase{2, 0, 0x3, false},
                                  DescriptorCase{4, 0x33, 0x33, true},
                                  DescriptorCase{6, 0x3f, 0x3f, true},
                                  DescriptorCase{6, 0x60f, 0x60f, true},
                                  DescriptorCase{8, 0x63f, 0x63f, true},
                                  DescriptorCase{4, 0, 0, false},
                                  DescriptorCase{6, 0, 0, false}};

    for (auto const& fixture : cases)
    {
      CAPTURE(fixture.sourceMask, fixture.channels);
      auto const application = PcmFormat{.sampleRate = 48000,
                                         .channels = fixture.channels,
                                         .encoding = SampleEncoding::Signed16Le,
                                         .speakerMask = fixture.sourceMask};
      auto const optWave = detail::waveFormatFromPcmFormat(application);
      REQUIRE(optWave);
      CHECK(optWave->dwChannelMask == fixture.nativeMask);
      CHECK(optWave->Format.nChannels == fixture.channels);
      CHECK(optWave->Format.nSamplesPerSec == 48000U);
      CHECK(optWave->Format.wBitsPerSample == 16U);
      CHECK(optWave->Format.nBlockAlign == fixture.channels * 2U);
      CHECK(optWave->Format.nAvgBytesPerSec == 48000U * fixture.channels * 2U);
      auto const observationRes = detail::observeWaveFormat(optWave->Format);
      REQUIRE(observationRes);
      REQUIRE(observationRes->optFormat);
      CHECK(observationRes->optFormat->speakerMask == fixture.nativeMask);
      CHECK(observationRes->optFormat->encoding == SampleEncoding::Signed16Le);
      auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optWave, {});
      CHECK(evidence.isMapped == fixture.isMapped);
      CHECK(evidence.nativeSpeakerMask == fixture.nativeMask);
      CHECK_FALSE(evidence.hasSharedConversion);
      CHECK(application.speakerMask == fixture.sourceMask);
    }
  }

  TEST_CASE("WasapiFormat - zero and FL FR stereo identities configure the same lossless native descriptors",
            "[audio][unit][wasapi][format]")
  {
    struct EncodingCase final
    {
      SampleEncoding encoding;
      WORD containerBits;
      WORD validBits;
      WORD blockAlign;
      DWORD byteRate;
      ::GUID subFormat;
    };
    // Literal 48000 Hz stereo geometry, independent of the encoding helpers
    // used to construct the descriptor. Every supported representation is here.
    auto const cases =
      std::array{EncodingCase{SampleEncoding::Signed16Le, 16, 16, 4, 192000, KSDATAFORMAT_SUBTYPE_PCM},
                 EncodingCase{SampleEncoding::Signed24PackedLe, 24, 24, 6, 288000, KSDATAFORMAT_SUBTYPE_PCM},
                 EncodingCase{SampleEncoding::Signed24In32Le, 32, 24, 8, 384000, KSDATAFORMAT_SUBTYPE_PCM},
                 EncodingCase{SampleEncoding::Signed32Le, 32, 32, 8, 384000, KSDATAFORMAT_SUBTYPE_PCM},
                 EncodingCase{SampleEncoding::Float32Le, 32, 32, 8, 384000, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT}};
    auto const mix = extensibleFloatStereoWave();
    auto const mixRes = detail::observeWaveFormat(mix.Format);
    REQUIRE(mixRes);
    REQUIRE(mixRes->optFormat);
    CHECK(mixRes->sampleRate == 48000U);
    CHECK(mixRes->channels == 2U);
    CHECK(mixRes->speakerMask == 3U);
    CHECK(mixRes->optFormat->encoding == SampleEncoding::Float32Le);

    for (auto const& fixture : cases)
    {
      for (auto const sourceMask : std::to_array<std::uint32_t>({0, 3}))
      {
        CAPTURE(fixture.encoding, sourceMask);
        auto const application =
          PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = fixture.encoding, .speakerMask = sourceMask};
        auto const optWave = detail::waveFormatFromPcmFormat(application);
        REQUIRE(optWave);
        CHECK(optWave->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE);
        CHECK(optWave->Format.cbSize == 22U);
        CHECK(optWave->Format.nSamplesPerSec == 48000U);
        CHECK(optWave->Format.nChannels == 2U);
        CHECK(optWave->dwChannelMask == 3U);
        CHECK(optWave->SubFormat == fixture.subFormat);
        CHECK(optWave->Format.wBitsPerSample == fixture.containerBits);
        // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
        CHECK(optWave->Samples.wValidBitsPerSample == fixture.validBits);
        CHECK(optWave->Format.nBlockAlign == fixture.blockAlign);
        CHECK(optWave->Format.nAvgBytesPerSec == fixture.byteRate);
        auto const observationRes = detail::observeWaveFormat(optWave->Format);
        REQUIRE(observationRes);
        CHECK(observationRes->sampleRate == 48000U);
        CHECK(observationRes->channels == 2U);
        CHECK(observationRes->speakerMask == 3U);
        REQUIRE(observationRes->optFormat);
        CHECK(observationRes->optFormat->sampleRate == 48000U);
        CHECK(observationRes->optFormat->channels == 2U);
        CHECK(observationRes->optFormat->encoding == fixture.encoding);
        CHECK(observationRes->optFormat->speakerMask == 3U);

        // These are fresh configured-client projections, not evidence updates
        // for gapless reuse. A zero source mask remains unidentified even when
        // its native descriptor matches the explicitly identified FL/FR source.
        auto const unavailableMixEvidence = detail::waveChannelDeliveryFromObservation(application, *optWave, {});
        CHECK(unavailableMixEvidence.isMapped == (sourceMask == 3U));
        CHECK(unavailableMixEvidence.nativeSpeakerMask == 3U);
        CHECK_FALSE(unavailableMixEvidence.hasSharedConversion);
        auto const observedMixEvidence = detail::waveChannelDeliveryFromObservation(application, *optWave, *mixRes);
        CHECK(observedMixEvidence.isMapped == (sourceMask == 3U));
        CHECK(observedMixEvidence.nativeSpeakerMask == 3U);
        CHECK_FALSE(observedMixEvidence.hasSharedConversion);
      }
    }
  }

  TEST_CASE("WasapiFormat - parses independent engine masks and keeps conversion separate from mapping",
            "[audio][unit][wasapi][format]")
  {
    // This is an independently authored engine descriptor, not a copy of the
    // source request. These model tests do not execute Initialize/GetMixFormat.
    auto mix = WAVEFORMATEXTENSIBLE{};
    mix.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mix.Format.nChannels = 2;
    mix.Format.nSamplesPerSec = 48000;
    mix.Format.wBitsPerSample = 32;
    mix.Format.nBlockAlign = 8;
    mix.Format.nAvgBytesPerSec = 384000;
    mix.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
    mix.Samples.wValidBitsPerSample = 32; // NOLINT(cppcoreguidelines-pro-type-union-access)
    mix.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    mix.dwChannelMask = 0x3;
    auto const mixRes = detail::observeWaveFormat(mix.Format);
    REQUIRE(mixRes);
    REQUIRE(mixRes->optFormat);
    CHECK(mixRes->speakerMask == 0x3U);
    CHECK(mixRes->optFormat->speakerMask == 0x3U);

    SECTION("alternate same-count set is a known shared conversion with mapped client roles")
    {
      auto const application =
        PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x30};
      auto const optWave = detail::waveFormatFromPcmFormat(application);
      REQUIRE(optWave);
      auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optWave, *mixRes);
      CHECK(evidence.isMapped);
      CHECK(evidence.hasSharedConversion);
      CHECK(optWave->dwChannelMask == 0x30U);
    }

    SECTION("a multichannel count conversion keeps the accepted client roles mapped")
    {
      auto const application =
        PcmFormat{.sampleRate = 48000, .channels = 6, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3f};
      auto const optWave = detail::waveFormatFromPcmFormat(application);
      REQUIRE(optWave);
      auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optWave, *mixRes);
      CHECK(evidence.isMapped);
      CHECK(evidence.hasSharedConversion);
      CHECK(optWave->dwChannelMask == 0x3fU);
    }

    SECTION("accepted centered mono can have independently observed FL-only engine conversion")
    {
      mix.Format.nChannels = 1;
      mix.Format.nBlockAlign = 4;
      mix.Format.nAvgBytesPerSec = 192000;
      mix.dwChannelMask = 1;
      auto const monoMixRes = detail::observeWaveFormat(mix.Format);
      REQUIRE(monoMixRes);
      REQUIRE(monoMixRes->optFormat);
      CHECK(monoMixRes->optFormat->speakerMask == 1U);
      auto const application =
        PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le, .speakerMask = 4};
      auto const optWave = detail::waveFormatFromPcmFormat(application);
      REQUIRE(optWave);
      auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optWave, *monoMixRes);
      CHECK(evidence.isMapped);
      CHECK(evidence.hasSharedConversion);
      CHECK(optWave->dwChannelMask == 4U);
    }

    SECTION("zero and FL FR source identities keep the same independently observed engine conversion")
    {
      mix.dwChannelMask = 0x30;
      auto const rearMixRes = detail::observeWaveFormat(mix.Format);
      REQUIRE(rearMixRes);
      CHECK(rearMixRes->sampleRate == 48000U);
      CHECK(rearMixRes->channels == 2U);
      CHECK(rearMixRes->speakerMask == 0x30U);
      REQUIRE(rearMixRes->optFormat);
      CHECK(rearMixRes->optFormat->encoding == SampleEncoding::Float32Le);
      CHECK(rearMixRes->optFormat->speakerMask == 0x30U);

      for (auto const sourceMask : std::to_array<std::uint32_t>({0, 3}))
      {
        CAPTURE(sourceMask);
        auto const application = PcmFormat{
          .sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = sourceMask};
        auto const optWave = detail::waveFormatFromPcmFormat(application);
        REQUIRE(optWave);
        CHECK(optWave->dwChannelMask == 3U);
        auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optWave, *rearMixRes);
        CHECK(evidence.isMapped == (sourceMask == 3U));
        CHECK(evidence.nativeSpeakerMask == 3U);
        CHECK(evidence.hasSharedConversion);
      }
    }

    SECTION("unknown engine mask remains unknown independently of the client request")
    {
      mix.dwChannelMask = 0;
      auto const unknownRes = detail::observeWaveFormat(mix.Format);
      REQUIRE(unknownRes);
      REQUIRE(unknownRes->optFormat);
      CHECK(unknownRes->speakerMask == 0U);
      CHECK(unknownRes->optFormat->speakerMask == 0U);
    }
  }

  TEST_CASE("WasapiFormat - exact client mapping requires equal accepted counts and role masks",
            "[audio][unit][wasapi][format]")
  {
    // The delivery check is a pure comparison against an accepted descriptor;
    // these sections vary that accepted descriptor away from the application
    // format without executing Initialize or GetMixFormat.
    auto const application =
      PcmFormat{.sampleRate = 48000, .channels = 6, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x60f};
    auto const optSideWave = detail::waveFormatFromPcmFormat(application);
    auto const optBackWave = detail::waveFormatFromPcmFormat(
      PcmFormat{.sampleRate = 48000, .channels = 6, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3f});
    REQUIRE(optSideWave);
    REQUIRE(optBackWave);

    SECTION("an exact accepted count and mask mark the client boundary mapped")
    {
      auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optSideWave, {});
      CHECK(evidence.isMapped);
      CHECK(evidence.nativeSpeakerMask == 0x60fU);
      CHECK_FALSE(evidence.hasSharedConversion);
    }

    SECTION("an accepted count mismatch alone stays unmapped")
    {
      // A pure accepted descriptor: the same 5.1 side role set with the count
      // raised to a literal valid 8ch geometry (S16, 48000 Hz). Its partial
      // role facts stay legal at 8 channels, no invalid source is built and
      // no native call runs; only the count guard can keep this unmapped.
      auto accepted = *optSideWave;
      accepted.Format.nChannels = 8;
      accepted.Format.nBlockAlign = 16;
      accepted.Format.nAvgBytesPerSec = 768000;
      auto const evidence = detail::waveChannelDeliveryFromObservation(application, accepted, {});
      CHECK_FALSE(evidence.isMapped);
      CHECK(evidence.nativeSpeakerMask == 0x60fU);
    }

    SECTION("an accepted mask mismatch with equal counts stays unmapped")
    {
      auto const evidence = detail::waveChannelDeliveryFromObservation(application, *optBackWave, {});
      CHECK_FALSE(evidence.isMapped);
      CHECK(evidence.nativeSpeakerMask == 0x3fU);
    }
  }

  TEST_CASE("WasapiFormat - unsafe descriptors cannot be constructed", "[audio][unit][wasapi][format]")
  {
    CHECK_FALSE(
      detail::waveFormatFromPcmFormat({.sampleRate = 0, .channels = 2, .encoding = SampleEncoding::Signed16Le}));
    CHECK_FALSE(
      detail::waveFormatFromPcmFormat({.sampleRate = 48000, .channels = 0, .encoding = SampleEncoding::Signed16Le}));
    CHECK_FALSE(
      detail::waveFormatFromPcmFormat({.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Unknown}));
    CHECK_FALSE(detail::waveFormatFromPcmFormat(
      {.sampleRate = 0xffffffff, .channels = 2, .encoding = SampleEncoding::Signed32Le}));
    CHECK_FALSE(detail::waveFormatFromPcmFormat(
      {.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 1}));
  }

  TEST_CASE("WasapiFormat - a short extensible descriptor is a storage fault that absent numerics cannot hide",
            "[audio][unit][wasapi][format]")
  {
    auto wave = extensibleFloatStereoWave();

    SECTION("a short successful extensible descriptor is refused before any extensible read")
    {
      wave.Format.cbSize = 0;
    }

    SECTION("zero dimensions cannot hide the short extensible object")
    {
      wave.Format.cbSize = 0;
      wave.Format.nSamplesPerSec = 0;
      wave.Format.nChannels = 0;
    }

    auto const observationRes = detail::observeWaveFormat(wave.Format);
    REQUIRE_FALSE(observationRes);
    CHECK(observationRes.error().code == Error::Code::FormatRejected);
    CHECK_FALSE(observationRes.error().message.empty());
  }

  TEST_CASE("WasapiFormat - absent mix numerics are an unavailable observation without guessed geometry",
            "[audio][unit][wasapi][format]")
  {
    auto wave = extensibleFloatStereoWave();

    SECTION("zero native count")
    {
      wave.Format.nChannels = 0;
    }

    SECTION("zero native rate")
    {
      wave.Format.nSamplesPerSec = 0;
    }

    SECTION("zero count and rate keep a complete mask unusable")
    {
      wave.Format.nChannels = 0;
      wave.Format.nSamplesPerSec = 0;
    }

    auto const observationRes = detail::observeWaveFormat(wave.Format);
    REQUIRE(observationRes);
    CHECK(observationRes->sampleRate == 0U);
    CHECK(observationRes->channels == 0U);
    CHECK(observationRes->speakerMask == 0U);
    CHECK_FALSE(observationRes->optFormat);
  }

  TEST_CASE("WasapiFormat - contradictory mix arithmetic withholds the concrete format but keeps facts",
            "[audio][unit][wasapi][format]")
  {
    auto wave = extensibleFloatStereoWave();
    std::uint32_t expectedMask = 3;

    SECTION("zero native stride")
    {
      wave.Format.nBlockAlign = 0;
    }

    SECTION("zero native byte rate")
    {
      wave.Format.nAvgBytesPerSec = 0;
    }

    SECTION("inconsistent native stride")
    {
      wave.Format.nBlockAlign = 4;
    }

    SECTION("inconsistent native byte rate")
    {
      wave.Format.nAvgBytesPerSec = 192000;
    }

    SECTION("a legal partial mask cannot hide inconsistent native stride")
    {
      wave.dwChannelMask = 1;
      wave.Format.nBlockAlign = 4;
      expectedMask = 1;
    }

    SECTION("a legal excess mask cannot hide inconsistent native byte rate")
    {
      wave.dwChannelMask = 7;
      wave.Format.nAvgBytesPerSec = 192000;
      expectedMask = 3;
    }

    SECTION("precision exceeds the container")
    {
      // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
      wave.Samples.wValidBitsPerSample = 33; // NOLINT(cppcoreguidelines-pro-type-union-access)
    }

    auto const observationRes = detail::observeWaveFormat(wave.Format);
    REQUIRE(observationRes);
    CHECK(observationRes->sampleRate == 48000U);
    CHECK(observationRes->channels == 2U);
    CHECK(observationRes->speakerMask == expectedMask);
    CHECK_FALSE(observationRes->optFormat);
  }

  TEST_CASE("WasapiFormat - reserved mix positions erase the role set instead of becoming legal",
            "[audio][unit][wasapi][format]")
  {
    auto wave = extensibleFloatStereoWave();
    bool hasConcreteFormat = true;

    SECTION("reserved positions remain outside supported native mask policy")
    {
      wave.dwChannelMask = 0x40000;
    }

    SECTION("wildcard positions remain outside supported native mask policy")
    {
      wave.dwChannelMask = 0x80000001;
    }

    SECTION("a reserved position alongside a complete set erases the whole role set")
    {
      wave.dwChannelMask = 0x40003;
    }

    SECTION("unsupported subtype cannot hide a reserved native mask")
    {
      wave.SubFormat = {};
      wave.dwChannelMask = 0x80000001;
      hasConcreteFormat = false;
    }

    auto const observationRes = detail::observeWaveFormat(wave.Format);
    REQUIRE(observationRes);
    CHECK(observationRes->sampleRate == 48000U);
    CHECK(observationRes->channels == 2U);
    // Reserved or wildcard positions are never declared legal or natively
    // mapped: the whole role set is erased and the canonical mask stays 0.
    CHECK(observationRes->speakerMask == 0U);
    CHECK(observationRes->optFormat.has_value() == hasConcreteFormat);

    if (observationRes->optFormat)
    {
      CHECK(observationRes->optFormat->sampleRate == 48000U);
      CHECK(observationRes->optFormat->channels == 2U);
      CHECK(observationRes->optFormat->encoding == SampleEncoding::Float32Le);
      CHECK(observationRes->optFormat->speakerMask == 0U);
    }
  }

  TEST_CASE("observeWaveFormat leaves unsupported endpoint encoding unknown", "[audio][unit][wasapi][format]")
  {
    auto wave = WAVEFORMATEX{};
    wave.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
    wave.nChannels = 2;
    wave.nSamplesPerSec = 48000;
    wave.wBitsPerSample = 16;
    wave.nBlockAlign = 1;
    wave.nAvgBytesPerSec = 16000;

    auto const observationRes = detail::observeWaveFormat(wave);
    REQUIRE(observationRes);
    CHECK(observationRes->channels == 2U);
    CHECK(observationRes->speakerMask == 0U);
    CHECK_FALSE(observationRes->optFormat);
  }

  TEST_CASE("observeWaveFormat keeps unknown subtype roles independent of PCM geometry",
            "[audio][unit][wasapi][format]")
  {
    auto wave = WAVEFORMATEXTENSIBLE{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = 2;
    wave.Format.nSamplesPerSec = 48000;
    wave.Format.wBitsPerSample = 32;
    wave.Format.nBlockAlign = 1;
    wave.Format.nAvgBytesPerSec = 16000;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
    wave.Samples.wValidBitsPerSample = 32; // NOLINT(cppcoreguidelines-pro-type-union-access)
    wave.SubFormat = {};
    wave.dwChannelMask = 0x3;
    // Unknown/non-PCM subtypes own their frame/block semantics. These literal
    // block and byte-rate values do not fit two-channel 32-bit PCM arithmetic;
    // only independent rate/count/roles survive, never a concrete PCM format.
    // Known PCM/float geometry failures remain covered by the contradictory-
    // arithmetic observation case, which also keeps independent facts.
    SECTION("non-PCM block semantics do not become a PCM geometry failure")
    {
    }

    SECTION("unknown container precision cannot produce a concrete format")
    {
      wave.Format.wBitsPerSample = 0;
    }

    SECTION("unknown subtype with zero valid precision still has no concrete format")
    {
      // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
      wave.Samples.wValidBitsPerSample = 0; // NOLINT(cppcoreguidelines-pro-type-union-access)
    }

    auto const observationRes = detail::observeWaveFormat(wave.Format);
    REQUIRE(observationRes);
    CHECK(observationRes->sampleRate == 48000U);
    CHECK(observationRes->channels == 2U);
    CHECK(observationRes->speakerMask == 0x3U);
    CHECK_FALSE(observationRes->optFormat);
  }

  TEST_CASE("wasapiInitializeFailureCause maps only the unsupported format to a route mismatch",
            "[audio][unit][wasapi][backend]")
  {
    struct CauseCase final
    {
      ::HRESULT status = S_OK;
      BackendOpenFailure::Cause cause = BackendOpenFailure::Cause::OutputFailure;
    };

    auto const causeCases =
      std::array{CauseCase{AUDCLNT_E_UNSUPPORTED_FORMAT, BackendOpenFailure::Cause::TrackRouteIncompatible},
                 CauseCase{E_INVALIDARG, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{E_OUTOFMEMORY, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{E_FAIL, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_DEVICE_INVALIDATED, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_ENDPOINT_CREATE_FAILED, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_DEVICE_IN_USE, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_SERVICE_NOT_RUNNING, BackendOpenFailure::Cause::OutputFailure},
                 CauseCase{AUDCLNT_E_RESOURCES_INVALIDATED, BackendOpenFailure::Cause::OutputFailure},
                 // A failing status unknown to the Initialize policy stays terminal.
                 CauseCase{E_NOTIMPL, BackendOpenFailure::Cause::OutputFailure}};

    for (auto const& causeCase : causeCases)
    {
      CAPTURE(causeCase.status);
      CHECK(detail::wasapiInitializeFailureCause(causeCase.status) == causeCase.cause);
    }
  }
} // namespace ao::audio::backend::test

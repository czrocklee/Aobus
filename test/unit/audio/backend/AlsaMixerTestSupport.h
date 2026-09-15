// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "lib/audio/backend/detail/AlsaGraphRegistry.h"
#include "lib/audio/backend/detail/AlsaMixerSession.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/SignalFormat.h>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ao::audio
{
  struct Device;
  class RenderTarget;
}

namespace ao::audio::backend::detail::test
{
  struct FakeMixerElement final
  {
    AlsaMixerElementId id;
    bool active = true;
    bool readable = true;
    AlsaMixerLevelRange rawRange{.min = 0L, .max = 100L};
    std::vector<long> rawLevels{100L};
    std::optional<AlsaMixerLevelRange> optDecibelRange{};
    std::vector<long> decibelLevels{};
    std::size_t generation = 1U;
  };

  struct FakeMixerState final
  {
    std::vector<FakeMixerElement> hardwareElements;
    std::vector<FakeMixerElement> cachedElements;
    bool openSucceeds = true;
    bool refreshSucceeds = true;
    bool writeSucceeds = true;
    bool applyFirstChannelBeforeWriteFailure = false;
    std::function<void()> beforeRefresh{};
    std::optional<bool> optHardwareMuted;
    std::optional<bool> optCachedHardwareMuted;
    std::size_t openCount = 0U;
    std::size_t closeCount = 0U;
    std::size_t refreshCount = 0U;
    std::size_t inspectCount = 0U;
    std::size_t writeCount = 0U;
    std::vector<std::size_t> writeGenerations;
    std::vector<long> writtenLevels;
  };

  class FakeMixerOpenFactory final : public AlsaMixerOpenFactory
  {
  public:
    explicit FakeMixerOpenFactory(std::shared_ptr<FakeMixerState> statePtr);

    std::unique_ptr<AlsaMixerIo> open(::snd_pcm_t* pcm) override;

  private:
    std::shared_ptr<FakeMixerState> _statePtr;
  };

  struct MixerFixture final
  {
    std::shared_ptr<FakeMixerState> statePtr = std::make_shared<FakeMixerState>();
    FakeMixerOpenFactory factory{statePtr};
    AlsaMixerSession session{factory};

    void addElement(FakeMixerElement element);
    void initialize();
  };

  /** @brief Fake-PCM backend that forwards controls through the concrete ALSA backend. */
  class AlsaControlBackend final : public Backend
  {
  public:
    AlsaControlBackend(Device const& device,
                       AlsaGraphPublisher publisher,
                       std::shared_ptr<FakeMixerState> mixerStatePtr);
    ~AlsaControlBackend() override;

    AlsaControlBackend(AlsaControlBackend const&) = delete;
    AlsaControlBackend& operator=(AlsaControlBackend const&) = delete;
    AlsaControlBackend(AlsaControlBackend&&) = delete;
    AlsaControlBackend& operator=(AlsaControlBackend&&) = delete;

    std::optional<PcmFormat> prewarmFormatHint(SignalFormat const& sourceFormat) const noexcept override;
    Result<OpenedPcmMode> open(SignalFormat const& sourceFormat, RenderTarget& target) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void stop() override;
    void close() override;

    Result<> setProperty(PropertyId id, PropertyValue const& value) override;
    Result<PropertyValue> property(PropertyId id) const override;
    PropertyInfo queryProperty(PropertyId id) const noexcept override;

    BackendId backendId() const override;
    ProfileId profileId() const override;
    bool isMixerInitialized() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio::backend::detail::test

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "AlsaGraphRegistry.h"
#include <ao/Error.h>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ao::audio::backend::detail
{
  /** @brief ALSA's native long-valued raw and dB scale, preserving the C API's platform width. */
  using AlsaMixerLevel = long;

  /** @brief Stable identity of one ALSA simple-mixer element. */
  struct AlsaMixerElementId final
  {
    std::string name;
    std::uint32_t index = 0U;

    bool operator==(AlsaMixerElementId const&) const = default;
  };

  /** @brief Inclusive native range for one mixer scale. */
  struct AlsaMixerLevelRange final
  {
    AlsaMixerLevel min = 0L;
    AlsaMixerLevel max = 0L;

    bool operator==(AlsaMixerLevelRange const&) const = default;
  };

  /**
   * @brief Fresh readable state for one active playback-volume element.
   *
   * A snapshot is valid only until the next refresh. The I/O boundary exposes
   * identities rather than native element pointers so every operation must
   * relocate an element after ALSA event handling.
   */
  struct AlsaMixerElementSnapshot final
  {
    AlsaMixerElementId id;
    AlsaMixerLevelRange rawRange;
    std::vector<AlsaMixerLevel> rawLevels;
    std::optional<AlsaMixerLevelRange> optDecibelRange;
    std::vector<AlsaMixerLevel> decibelLevels;
    std::optional<bool> optHardwareMuted;
  };

  /** @brief One mutex-consistent control snapshot for graph publication. */
  struct AlsaMixerStateSnapshot final
  {
    float volume = 1.0F;
    bool applicationMuted = false;
    bool effectiveMuted = false;
    AlsaVolumeControlMode volumeMode = AlsaVolumeControlMode::Unavailable;
  };

  /** @brief Narrow I/O owned by one open ALSA mixer conversation. */
  class AlsaMixerIo
  {
  public:
    virtual ~AlsaMixerIo() = default;

    AlsaMixerIo(AlsaMixerIo const&) = delete;
    AlsaMixerIo& operator=(AlsaMixerIo const&) = delete;
    AlsaMixerIo(AlsaMixerIo&&) = delete;
    AlsaMixerIo& operator=(AlsaMixerIo&&) = delete;

    virtual bool tryRefresh() = 0;
    virtual std::vector<AlsaMixerElementId> elementIds() const = 0;
    virtual std::optional<AlsaMixerElementSnapshot> inspect(AlsaMixerElementId const& id) const = 0;
    virtual bool trySetRawVolume(AlsaMixerElementId const& id, AlsaMixerLevel level) = 0;
    virtual bool trySetDecibelVolume(AlsaMixerElementId const& id, AlsaMixerLevel level) = 0;

  protected:
    AlsaMixerIo() = default;
  };

  /** @brief Opens mixer I/O associated with one already-open PCM device. */
  class AlsaMixerOpenFactory
  {
  public:
    virtual ~AlsaMixerOpenFactory() = default;

    AlsaMixerOpenFactory(AlsaMixerOpenFactory const&) = delete;
    AlsaMixerOpenFactory& operator=(AlsaMixerOpenFactory const&) = delete;
    AlsaMixerOpenFactory(AlsaMixerOpenFactory&&) = delete;
    AlsaMixerOpenFactory& operator=(AlsaMixerOpenFactory&&) = delete;

    virtual std::unique_ptr<AlsaMixerIo> open(::snd_pcm_t* pcm) = 0;

  protected:
    AlsaMixerOpenFactory() = default;
  };

  AlsaMixerOpenFactory& nativeAlsaMixerOpenFactory();

  /**
   * @brief Owns safe volume state across one ALSA backend's PCM open/close cycles.
   *
   * Initialization and close are read-only with respect to shared mixer
   * controls. Hardware reads and writes refresh events and relocate the chosen
   * element by stable identity. Application mute is always software-only.
   */
  class [[nodiscard]] AlsaMixerSession final
  {
  public:
    /// The borrowed factory must outlive this session.
    explicit AlsaMixerSession(AlsaMixerOpenFactory& factory);

    // Success selects a fresh readable hardware route; it does not certify
    // write capability. The first explicit volume request observes that result.
    // The PCM handle is borrowed for this call only; the session does not retain it.
    bool tryInit(::snd_pcm_t* pcm);
    void close();

    Result<> setVolume(float volume);
    AlsaMixerStateSnapshot setMuted(bool muted);
    AlsaMixerStateSnapshot stateSnapshot();
    bool isApplicationMuted() const;

    AlsaVolumeControlMode volumeMode() const noexcept;
    float renderGain() const noexcept;

  private:
    std::optional<AlsaMixerElementSnapshot> refreshSelected();
    AlsaMixerStateSnapshot stateSnapshotLocked();
    void useSoftwareGainAfterHardwareUncertainty() noexcept;
    void publishRenderGainLocked() noexcept;

    AlsaMixerOpenFactory& _factory;
    mutable std::mutex _handleMutex;
    std::unique_ptr<AlsaMixerIo> _ioPtr;
    std::optional<AlsaMixerElementId> _optElementId;
    // A setter failure makes this stable element identity unsafe for the rest of the backend lifetime.
    std::vector<AlsaMixerElementId> _failedElementIds;

    // Control values are protected by _handleMutex; render never reads them directly.
    float _softwareVolume = 1.0F;
    bool _applicationMuted = false;

    // Only capabilities and the complete derived render gain are read outside _handleMutex.
    std::atomic<AlsaVolumeControlMode> _volumeMode{AlsaVolumeControlMode::Unavailable};
    std::atomic<float> _renderGain{1.0F};
    static_assert(std::atomic<float>::is_always_lock_free, "Realtime gain reads must remain lock-free");
  };
} // namespace ao::audio::backend::detail

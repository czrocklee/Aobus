// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmSource.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

namespace ao::audio
{
  struct PlaybackInput;
}

namespace ao::audio::detail
{
  /**
   * @brief Manages a single track playback session, including decoder lifecycle and format negotiation.
   */
  class TrackSession final
  {
  public:
    using DecoderFactoryFn =
      std::function<std::unique_ptr<DecoderSession>(std::filesystem::path const&, Format const&)>;
    using OnSourceErrorFn = std::function<void(Error const&)>;

    struct OpenedTrack
    {
      std::shared_ptr<PcmSource> sourcePtr;
      Format backendFormat;
      DecodedStreamInfo info;
    };

    /**
     * @brief Creates a new track session by opening and negotiating the decoder.
     */
    static Result<OpenedTrack> create(PlaybackInput const& input,
                                      Device const& device,
                                      BackendId const& backendId,
                                      ProfileId const& profileId,
                                      DecoderFactoryFn const& decoderFactory,
                                      OnSourceErrorFn onSourceError,
                                      std::chrono::milliseconds initialOffset = {});

  private:
    static void negotiateFormat(std::filesystem::path const& path,
                                DecodedStreamInfo& info,
                                std::unique_ptr<DecoderSession>& decoderPtr,
                                Format& backendFormat,
                                Device const& device,
                                BackendId const& backendId,
                                ProfileId const& profileId,
                                DecoderFactoryFn const& decoderFactory);

    static std::shared_ptr<PcmSource> createPcmSource(std::unique_ptr<DecoderSession> decoderPtr,
                                                      DecodedStreamInfo const& info,
                                                      OnSourceErrorFn onSourceError);
  };
} // namespace ao::audio::detail

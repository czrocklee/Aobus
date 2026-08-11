// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "../PcmSource.h"
#include "../StreamingSource.h"
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace ao::audio
{
  struct PlaybackInput;
}

namespace ao::audio::detail
{
  /**
   * @brief Manages inspection, exact decoder output preparation, and activation for one track.
   */
  class TrackSession final
  {
  public:
    using DecoderFactoryFn = std::function<Result<std::unique_ptr<DecoderSession>>(std::filesystem::path const&,
                                                                                   std::optional<SampleEncoding>)>;
    using OnSourceErrorFn = std::function<void(Error const&)>;

    struct Inspection
    {
      DecodedStreamInfo info;
    };

    struct OpenedTrack
    {
      std::shared_ptr<PcmSource> sourcePtr;
      PcmFormat backendFormat;
      DecodedStreamInfo info;
    };

    struct PreparedTrack
    {
      std::unique_ptr<StreamingSource> sourcePtr;
      PcmFormat backendFormat;
      DecodedStreamInfo info;
    };

    static Result<Inspection> inspect(PlaybackInput const& input, DecoderFactoryFn const& decoderFactory);

    static Result<PreparedTrack> prepare(PlaybackInput const& input,
                                         Inspection const& inspection,
                                         PcmFormat const& backendFormat,
                                         DecoderFactoryFn const& decoderFactory,
                                         std::chrono::milliseconds initialOffset = {});

    static OpenedTrack activate(PreparedTrack preparedTrack, OnSourceErrorFn onSourceError);

  private:
    static std::unique_ptr<StreamingSource> preparePcmSource(std::unique_ptr<DecoderSession> decoderPtr,
                                                             DecodedStreamInfo const& info);
  };
} // namespace ao::audio::detail

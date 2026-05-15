// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/ISource.h>
#include <ao/audio/Types.h>
#include <functional>
#include <memory>

namespace ao::audio::detail
{
  /**
   * @brief Manages a single track playback session, including decoder lifecycle and format negotiation.
   */
  class TrackSession final
  {
  public:
    using DecoderFactoryFn =
      std::function<std::unique_ptr<IDecoderSession>(std::filesystem::path const&, Format const&)>;
    using OnSourceErrorFn = std::function<void(Error const&)>;

    struct Result
    {
      std::shared_ptr<ISource> source = nullptr;
      Format backendFormat{};
      DecodedStreamInfo info{};
      Error error{};

      explicit operator bool() const noexcept { return source != nullptr; }
    };

    /**
     * @brief Creates a new track session by opening and negotiating the decoder.
     */
    static Result create(TrackPlaybackDescriptor const& descriptor,
                         Device const& device,
                         BackendId const& backendId,
                         ProfileId const& profileId,
                         DecoderFactoryFn const& decoderFactory,
                         OnSourceErrorFn onSourceError);

  private:
    static bool negotiateFormat(std::filesystem::path const& path,
                                DecodedStreamInfo& info,
                                std::unique_ptr<IDecoderSession>& decoder,
                                Format& backendFormat,
                                Device const& device,
                                BackendId const& backendId,
                                ProfileId const& profileId,
                                DecoderFactoryFn const& decoderFactory,
                                std::string& errorMsg);

    static std::shared_ptr<ISource> createPcmSource(std::unique_ptr<IDecoderSession> decoder,
                                                    DecodedStreamInfo const& info,
                                                    OnSourceErrorFn onSourceError,
                                                    std::string& errorMsg);

    static bool shouldUseMemoryPcmSource(DecodedStreamInfo const& info);
  };
} // namespace ao::audio::detail

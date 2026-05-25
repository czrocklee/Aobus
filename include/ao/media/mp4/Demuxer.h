// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/Error.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ao::media::mp4
{
  /**
   * @brief Demuxer for extracting specific streams (like ALAC/AAC packets) from an MP4 file container.
   */
  class Demuxer final
  {
  public:
    struct SampleEntry final
    {
      std::uint64_t offset = 0;
      std::uint32_t size = 0;
      std::uint64_t startTime = 0;
      std::uint32_t duration = 0;
    };

    /**
     * @brief Construct an MP4 Demuxer over the given span of raw container bytes.
     */
    explicit Demuxer(std::span<std::byte const> fileData);
    ~Demuxer() = default;

    Demuxer(Demuxer const&) = delete;
    Demuxer& operator=(Demuxer const&) = delete;
    Demuxer(Demuxer&&) = delete;
    Demuxer& operator=(Demuxer&&) = delete;

    /**
     * @brief Parses the MP4 atoms, looking for an audio track matching the given format (e.g. "alac").
     * @return Result of the operation.
     */
    Result<> parseTrack(std::string_view targetFormat);

    /**
     * @brief Returns the codec-specific magic cookie/extradata found during parsing.
     */
    std::span<std::byte const> magicCookie() const;

    /**
     * @brief Returns the total number of audio samples.
     */
    std::uint32_t sampleCount() const;

    /**
     * @brief Gets the offset and size information for a given sample index.
     */
    SampleEntry getSampleInfo(std::uint32_t index) const;

    /**
     * @brief Retrieves the payload bytes for a given sample index directly from the file buffer.
     * @return A span of bytes, or an empty span if index is out of range or bytes are missing.
     */
    std::span<std::byte const> getSamplePayload(std::uint32_t index) const;

    /**
     * @brief Maps a media timestamp in mdhd timescale units to the sample packet containing it.
     * @return The packet index, or sampleCount() when the timestamp is at or beyond the end.
     */
    std::uint32_t sampleIndexAtTime(std::uint64_t time) const noexcept;

    /**
     * @brief The timescale parsed from the media header (mdhd). Useful for calculating durations.
     */
    std::uint32_t timescale() const;

    /**
     * @brief The track duration parsed from the media header (mdhd), in timescale units.
     */
    std::uint64_t duration() const;

  private:
    struct SampleToChunkEntry final
    {
      std::uint32_t firstChunk = 0;
      std::uint32_t samplesPerChunk = 0;
    };

    struct TimeToSampleEntry final
    {
      std::uint32_t sampleCount = 0;
      std::uint32_t sampleDelta = 0;
    };

    void parseStts(std::span<std::byte const> bytes, std::vector<TimeToSampleEntry>& out);
    void parseStsz(std::span<std::byte const> bytes);
    void parseStsc(std::span<std::byte const> bytes, std::vector<SampleToChunkEntry>& out);
    void parseStco(std::span<std::byte const> bytes, std::vector<std::uint64_t>& out);
    void parseCo64(std::span<std::byte const> bytes, std::vector<std::uint64_t>& out);

    static bool applySampleTiming(std::vector<SampleEntry>& samples, std::span<TimeToSampleEntry const> timeToSample);
    static bool buildSampleOffsets(std::vector<SampleEntry>& samples,
                                   std::span<std::uint64_t const> chunkOffsets,
                                   std::span<SampleToChunkEntry const> sampleToChunk);

    std::span<std::byte const> _fileData;
    std::vector<std::byte> _magicCookie;
    std::vector<SampleEntry> _samples;

    std::uint32_t _timescale = 0;
    std::uint64_t _duration = 0;
  };
} // namespace ao::media::mp4

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "PlaybackTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace app::playback
{

  struct PcmBlock final
  {
    std::vector<std::byte> bytes;
    std::uint8_t bitDepth = 16;
    std::uint32_t frames = 0;
    std::uint64_t firstFrameIndex = 0;
    bool endOfStream = false;
  };

  struct DecodedStreamInfo final
  {
    StreamFormat sourceFormat;
    StreamFormat outputFormat;
    std::uint32_t durationMs = 0;
  };

  class FfmpegDecoderSession final
  {
  public:
    explicit FfmpegDecoderSession(StreamFormat outputFormat);
    ~FfmpegDecoderSession();

    bool open(std::filesystem::path const& filePath);
    void close();
    void seek(std::uint32_t positionMs);
    void flush();

    std::optional<PcmBlock> readNextBlock();
    DecodedStreamInfo streamInfo() const;
    std::string_view lastError() const noexcept;

  private:
    struct FormatContextDeleter
    {
      void operator()(AVFormatContext* ptr) const noexcept;
    };

    struct CodecContextDeleter
    {
      void operator()(AVCodecContext* ptr) const noexcept;
    };

    struct PacketDeleter
    {
      void operator()(AVPacket* ptr) const noexcept;
    };

    struct FrameDeleter
    {
      void operator()(AVFrame* ptr) const noexcept;
    };

    struct SwrContextDeleter
    {
      void operator()(SwrContext* ptr) const noexcept;
    };

    using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
    using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
    using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

    bool openInput(std::filesystem::path const& filePath);
    bool openAudioStream();
    bool configureResampler();
    std::optional<PcmBlock> convertFrameToInterleavedPcm();
    std::uint32_t getCurrentPositionMs() const;
    void setError(std::string message);

    StreamFormat _outputFormat;
    DecodedStreamInfo _streamInfo;
    FormatContextPtr _formatContext;
    CodecContextPtr _codecContext;
    PacketPtr _packet;
    FramePtr _frame;
    SwrContextPtr _swrContext;
    int _audioStreamIndex = -1;
    bool _inputEof = false;
    bool _decoderEof = false;
    bool _flushPacketSent = false;
    std::uint64_t _decodedFrameCursor = 0;
    std::string _lastError;
  };

} // namespace app::playback

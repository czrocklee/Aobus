// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/AudioDecoderSession.h"

#include <memory>
#include <string>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace app::core::playback
{

  class FfmpegDecoderSession final : public IAudioDecoderSession
  {
  public:
    static void initGlobal();

    explicit FfmpegDecoderSession(StreamFormat outputFormat);
    ~FfmpegDecoderSession() override;

    FfmpegDecoderSession(FfmpegDecoderSession const&) = delete;
    FfmpegDecoderSession& operator=(FfmpegDecoderSession const&) = delete;
    FfmpegDecoderSession(FfmpegDecoderSession&&) noexcept = default;
    FfmpegDecoderSession& operator=(FfmpegDecoderSession&&) noexcept = default;

    bool open(std::filesystem::path const& filePath) override;
    void close() override;
    bool seek(std::uint32_t positionMs) override;
    void flush() override;

    std::optional<PcmBlock> readNextBlock() override;
    DecodedStreamInfo streamInfo() const override;
    std::string_view lastError() const noexcept override;

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
    std::int32_t _audioStreamIndex = -1;
    bool _inputEof = false;
    bool _decoderEof = false;
    bool _flushPacketSent = false;
    std::uint64_t _decodedFrameCursor = 0;
    std::string _lastError;
  };

} // namespace app::core::playback

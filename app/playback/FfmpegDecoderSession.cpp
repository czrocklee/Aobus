// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "FfmpegDecoderSession.h"

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>

namespace app::playback
{

  // Deleter implementations
  void FfmpegDecoderSession::FormatContextDeleter::operator()(AVFormatContext* ptr) const noexcept
  {
    if (ptr) { avformat_close_input(&ptr); }
  }

  void FfmpegDecoderSession::CodecContextDeleter::operator()(AVCodecContext* ptr) const noexcept
  {
    if (ptr) { avcodec_free_context(&ptr); }
  }

  void FfmpegDecoderSession::PacketDeleter::operator()(AVPacket* ptr) const noexcept
  {
    if (ptr) { av_packet_free(&ptr); }
  }

  void FfmpegDecoderSession::FrameDeleter::operator()(AVFrame* ptr) const noexcept
  {
    if (ptr) { av_frame_free(&ptr); }
  }

  void FfmpegDecoderSession::SwrContextDeleter::operator()(SwrContext* ptr) const noexcept
  {
    if (ptr) { swr_free(&ptr); }
  }

  FfmpegDecoderSession::FfmpegDecoderSession(StreamFormat outputFormat)
    : _outputFormat(outputFormat)
  {
  }

  FfmpegDecoderSession::~FfmpegDecoderSession() = default;

  void FfmpegDecoderSession::open(std::filesystem::path const& filePath)
  {
    close();

    // Open input file
    openInput(filePath);

    // Find and open audio stream
    openAudioStream();

    // Configure resampler for planar to interleaved conversion
    configureResampler();

    // Get duration
    if (_formatContext && _formatContext->duration != AV_NOPTS_VALUE)
    {
      _streamInfo.durationMs = static_cast<std::uint32_t>(
        _formatContext->duration / (AV_TIME_BASE / 1000));
    }
  }

  void FfmpegDecoderSession::close()
  {
    _formatContext.reset();
    _codecContext.reset();
    _packet.reset();
    _frame.reset();
    _swrContext.reset();
    _audioStreamIndex = -1;
    _inputEof = false;
    _decoderEof = false;
    _decodedFrameCursor = 0;
    _streamInfo = {};
  }

  void FfmpegDecoderSession::openInput(std::filesystem::path const& filePath)
  {
    AVFormatContext* ctx = nullptr;

    int ret = avformat_open_input(&ctx, filePath.string().c_str(), nullptr, nullptr);
    if (ret < 0)
    {
      std::cerr << "Failed to open input: " << filePath << std::endl;
      std::cerr << "FFmpeg error: " << av_err2str(ret) << " (code=" << ret << ")" << std::endl;
      return;
    }

    _formatContext.reset(ctx);

    ret = avformat_find_stream_info(ctx, nullptr);
    if (ret < 0)
    {
      std::cerr << "Failed to find stream info" << std::endl;
      return;
    }

    av_dump_format(ctx, 0, filePath.string().c_str(), 0);
  }

  void FfmpegDecoderSession::openAudioStream()
  {
    if (!_formatContext) { return; }

    // Find audio stream
    _audioStreamIndex = av_find_best_stream(_formatContext.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (_audioStreamIndex < 0)
    {
      std::cerr << "Failed to find audio stream" << std::endl;
      return;
    }

    auto* stream = _formatContext->streams[_audioStreamIndex];

    // Find decoder
    auto const* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec)
    {
      std::cerr << "Failed to find codec" << std::endl;
      return;
    }

    // Create codec context
    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
    {
      std::cerr << "Failed to allocate codec context" << std::endl;
      return;
    }

    _codecContext.reset(codecCtx);

    // Copy codec parameters to context
    int ret = avcodec_parameters_to_context(_codecContext.get(), stream->codecpar);
    if (ret < 0)
    {
      std::cerr << "Failed to copy codec parameters" << std::endl;
      return;
    }

    // Open codec
    ret = avcodec_open2(_codecContext.get(), codec, nullptr);
    if (ret < 0)
    {
      std::cerr << "Failed to open codec" << std::endl;
      return;
    }

    // Allocate packet and frame
    _packet.reset(av_packet_alloc());
    _frame.reset(av_frame_alloc());

    // Store source format info
    _streamInfo.sourceFormat.sampleRate = _codecContext->sample_rate;
    _streamInfo.sourceFormat.channels = static_cast<std::uint8_t>(_codecContext->ch_layout.nb_channels);
    _streamInfo.sourceFormat.bitDepth = static_cast<std::uint8_t>(av_get_bytes_per_sample(_codecContext->sample_fmt) * 8);
    _streamInfo.sourceFormat.isFloat = av_sample_fmt_is_planar(_codecContext->sample_fmt) != 0;
    _streamInfo.sourceFormat.isInterleaved = !av_sample_fmt_is_planar(_codecContext->sample_fmt);

    // Output format - use hint if provided, otherwise match source (resampler handles conversion)
    _streamInfo.outputFormat = _outputFormat;
    if (_outputFormat.sampleRate == 0 || _outputFormat.sampleRate == 44100) { _streamInfo.outputFormat.sampleRate = _streamInfo.sourceFormat.sampleRate; }
    if (_outputFormat.channels == 0) { _streamInfo.outputFormat.channels = _streamInfo.sourceFormat.channels; }
    if (_outputFormat.bitDepth == 0 || _outputFormat.bitDepth == 16) { _streamInfo.outputFormat.bitDepth = _streamInfo.sourceFormat.bitDepth; }
  }

  void FfmpegDecoderSession::configureResampler()
  {
    if (!_codecContext) { return; }

    // Determine output sample format (always interleaved, S16 for spsc_queue)
    auto const outSampleFormat = AVSampleFormat::AV_SAMPLE_FMT_S16;

    auto inChannelLayout = AVChannelLayout{};
    if (_codecContext->ch_layout.nb_channels > 0 && av_channel_layout_check(&_codecContext->ch_layout) != 0)
    {
      av_channel_layout_copy(&inChannelLayout, &_codecContext->ch_layout);
    }
    else
    {
      av_channel_layout_default(&inChannelLayout, _streamInfo.outputFormat.channels);
    }

    auto outChannelLayout = AVChannelLayout{};
    av_channel_layout_default(&outChannelLayout, _streamInfo.outputFormat.channels);

    // Create resampler context
    SwrContext* swrCtx = swr_alloc();
    if (!swrCtx)
    {
      av_channel_layout_uninit(&inChannelLayout);
      av_channel_layout_uninit(&outChannelLayout);
      std::cerr << "Failed to allocate resampler" << std::endl;
      return;
    }

    // Set options
    av_opt_set_chlayout(swrCtx, "in_chlayout", &inChannelLayout, 0);
    av_opt_set_int(swrCtx, "in_sample_rate", _codecContext->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", _codecContext->sample_fmt, 0);
    av_opt_set_chlayout(swrCtx, "out_chlayout", &outChannelLayout, 0);
    av_opt_set_int(swrCtx, "out_sample_rate", _streamInfo.outputFormat.sampleRate, 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", outSampleFormat, 0);

    // Initialize
    int ret = swr_init(swrCtx);
    av_channel_layout_uninit(&inChannelLayout);
    av_channel_layout_uninit(&outChannelLayout);
    if (ret < 0)
    {
      std::cerr << "Failed to initialize resampler: " << av_err2str(ret) << std::endl;
      swr_free(&swrCtx);
      return;
    }

    _swrContext.reset(swrCtx);
  }

  void FfmpegDecoderSession::seek(std::uint32_t positionMs)
  {
    if (!_formatContext || _audioStreamIndex < 0) { return; }

    auto const* stream = _formatContext->streams[_audioStreamIndex];
    auto const timestamp = av_rescale_q(static_cast<std::int64_t>(positionMs), {1, 1000}, stream->time_base);
    auto const seekFlags = positionMs > getCurrentPositionMs() ? 0 : AVSEEK_FLAG_BACKWARD;

    int ret = av_seek_frame(_formatContext.get(), _audioStreamIndex, timestamp, seekFlags);
    if (ret < 0)
    {
      std::cerr << "Seek failed (ret=" << ret << ", pos=" << positionMs << "ms)" << std::endl;
      return;
    }

    // Flush decoder
    flush();

    // Reset frame cursor
    _decodedFrameCursor = 0;
  }

  void FfmpegDecoderSession::flush()
  {
    if (_codecContext)
    {
      avcodec_flush_buffers(_codecContext.get());
    }
    _decoderEof = false;
    _inputEof = false;
  }

  std::uint32_t FfmpegDecoderSession::getCurrentPositionMs() const
  {
    if (!_formatContext || !_codecContext || !_frame) { return 0; }
    auto const stream = _formatContext->streams[_audioStreamIndex];
    if (!stream) { return 0; }

    auto const base = stream->time_base;
    auto const pts = _frame->pts;
    if (pts == AV_NOPTS_VALUE) { return 0; }

    return static_cast<std::uint32_t>(av_rescale_q(pts, base, {1, 1000}));
  }

  std::optional<PcmBlock> FfmpegDecoderSession::readNextBlock()
  {
    if (!_formatContext || !_codecContext || !_packet || !_frame)
    {
      return std::nullopt;
    }

    // Try to drain decoder first
    if (_decoderEof)
    {
      PcmBlock block;
      block.endOfStream = true;
      return block;
    }

    // Read packets until we have decoded frames
    while (true)
    {
      // If decoder needs more input
      if (!_inputEof)
      {
        int ret = av_read_frame(_formatContext.get(), _packet.get());
        if (ret == AVERROR_EOF)
        {
          _inputEof = true;
          // Send null packet to flush decoder
          _packet->data = nullptr;
          _packet->size = 0;
        }
        else if (ret < 0)
        {
          std::cerr << "Error reading frame" << std::endl;
          _inputEof = true;
          return std::nullopt;
        }
        else if (_packet->stream_index != _audioStreamIndex)
        {
          // Not our stream, continue
          continue;
        }

        // Send packet to decoder
        ret = avcodec_send_packet(_codecContext.get(), _packet.get());
        if (ret < 0)
        {
          std::cerr << "Error sending packet to decoder" << std::endl;
          return std::nullopt;
        }

        // Don't free packet here, decoder references it
      }
      else
      {
        // Send null packet to flush decoder
        _packet->data = nullptr;
        _packet->size = 0;
        int ret = avcodec_send_packet(_codecContext.get(), _packet.get());
        if (ret < 0)
        {
          _decoderEof = true;
          PcmBlock block;
          block.endOfStream = true;
          return block;
        }
      }

      // Receive frames from decoder
      while (true)
      {
        int ret = avcodec_receive_frame(_codecContext.get(), _frame.get());
        if (ret == AVERROR_EOF)
        {
          _decoderEof = true;
          PcmBlock block;
          block.endOfStream = true;
          return block;
        }
        else if (ret == AVERROR(EAGAIN))
        {
          // Need more input
          break;
        }
        else if (ret < 0)
        {
          std::cerr << "Error receiving frame from decoder" << std::endl;
          return std::nullopt;
        }

        // Convert frame to interleaved PCM
        auto block = convertFrameToInterleavedPcm();
        if (block && block->frames > 0)
        {
          return block;
        }
      }
    }
  }

  std::optional<PcmBlock> FfmpegDecoderSession::convertFrameToInterleavedPcm()
  {
    if (!_frame || !_swrContext) { return std::nullopt; }

    auto const outChannels = _streamInfo.outputFormat.channels;

    // Calculate output buffer size in samples
    auto const outSamples = av_rescale_rnd(
      swr_get_delay(_swrContext.get(), _frame->sample_rate) + _frame->nb_samples,
      _streamInfo.outputFormat.sampleRate,
      _frame->sample_rate,
      AVRounding::AV_ROUND_UP);

    if (outSamples <= 0) { return std::nullopt; }

    std::vector<std::int16_t> outBuffer(outSamples * outChannels);

    // Convert
    auto* outPtr = reinterpret_cast<std::uint8_t*>(outBuffer.data());
    int const convertedSamples = swr_convert(
      _swrContext.get(),
      &outPtr,
      static_cast<int>(outSamples),
      const_cast<std::uint8_t const**>(_frame->data),
      _frame->nb_samples);

    if (convertedSamples <= 0) { return std::nullopt; }

    PcmBlock block;
    block.samples = std::move(outBuffer);
    block.frames = static_cast<std::uint32_t>(convertedSamples);
    block.firstFrameIndex = _decodedFrameCursor;
    block.endOfStream = false;

    // Update cursor (estimate based on sample rate ratio)
    auto const durationRatio = static_cast<double>(_streamInfo.outputFormat.sampleRate) / _frame->sample_rate;
    _decodedFrameCursor += static_cast<std::uint64_t>(_frame->nb_samples * durationRatio);

    return block;
  }

  DecodedStreamInfo FfmpegDecoderSession::streamInfo() const
  {
    return _streamInfo;
  }

} // namespace app::playback

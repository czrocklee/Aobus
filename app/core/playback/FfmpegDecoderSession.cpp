// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/FfmpegDecoderSession.h"
#include "core/Log.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstring>

namespace
{
  void ffmpegLogCallback(void* ptr, int level, char const* fmt, va_list vl)
  {
    if (level > av_log_get_level())
    {
      return;
    }

    char line[1024];
    int print_prefix = 1;
    ::av_log_format_line(ptr, level, fmt, vl, line, sizeof(line), &print_prefix);

    // Remove trailing newline if present to let spdlog handle formatting
    std::string message(line);

    if (!message.empty() && message.back() == '\n')
    {
      message.pop_back();
    }

    if (message.empty())
    {
      return;
    }

    switch (level)
    {
      case AV_LOG_PANIC:
      case AV_LOG_FATAL: PLAYBACK_LOG_CRITICAL("[ffmpeg] {}", message); break;
      case AV_LOG_ERROR: PLAYBACK_LOG_ERROR("[ffmpeg] {}", message); break;
      case AV_LOG_WARNING: PLAYBACK_LOG_WARN("[ffmpeg] {}", message); break;
      case AV_LOG_INFO: PLAYBACK_LOG_INFO("[ffmpeg] {}", message); break;
      case AV_LOG_VERBOSE:
      case AV_LOG_DEBUG: PLAYBACK_LOG_DEBUG("[ffmpeg] {}", message); break;
      case AV_LOG_TRACE: PLAYBACK_LOG_TRACE("[ffmpeg] {}", message); break;
      default: PLAYBACK_LOG_DEBUG("[ffmpeg] {}", message); break;
    }
  }

  std::string ffmpegErrorText(std::int32_t errorCode)
  {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    ::av_strerror(errorCode, buffer, sizeof(buffer));
    return buffer;
  }

  bool isFloatSampleFormat(AVSampleFormat sampleFormat)
  {
    switch (sampleFormat)
    {
      case AV_SAMPLE_FMT_FLT:
      case AV_SAMPLE_FMT_FLTP:
      case AV_SAMPLE_FMT_DBL:
      case AV_SAMPLE_FMT_DBLP: return true;
      default: return false;
    }
  }
} // namespace

namespace app::core::playback
{

  // Deleter implementations
  void FfmpegDecoderSession::FormatContextDeleter::operator()(AVFormatContext* ptr) const noexcept
  {
    if (ptr)
    {
      ::avformat_close_input(&ptr);
    }
  }

  void FfmpegDecoderSession::CodecContextDeleter::operator()(AVCodecContext* ptr) const noexcept
  {
    if (ptr)
    {
      ::avcodec_free_context(&ptr);
    }
  }

  void FfmpegDecoderSession::PacketDeleter::operator()(AVPacket* ptr) const noexcept
  {
    if (ptr)
    {
      ::av_packet_free(&ptr);
    }
  }

  void FfmpegDecoderSession::FrameDeleter::operator()(AVFrame* ptr) const noexcept
  {
    if (ptr)
    {
      ::av_frame_free(&ptr);
    }
  }

  void FfmpegDecoderSession::SwrContextDeleter::operator()(SwrContext* ptr) const noexcept
  {
    if (ptr)
    {
      ::swr_free(&ptr);
    }
  }

  void FfmpegDecoderSession::initGlobal()
  {
    ::av_log_set_callback(ffmpegLogCallback);
  }

  FfmpegDecoderSession::FfmpegDecoderSession(StreamFormat outputFormat)
    : _outputFormat{outputFormat}
  {
  }

  FfmpegDecoderSession::~FfmpegDecoderSession() = default;

  bool FfmpegDecoderSession::open(std::filesystem::path const& filePath)
  {
    close();

    // Open input file

    if (!openInput(filePath))
    {
      return false;
    }

    // Find and open audio stream

    if (!openAudioStream())
    {
      return false;
    }

    // Configure resampler for planar to interleaved conversion

    if (!configureResampler())
    {
      return false;
    }

    // Get duration

    if (_formatContext && _formatContext->duration != AV_NOPTS_VALUE)
    {
      _streamInfo.durationMs = static_cast<std::uint32_t>(_formatContext->duration / (AV_TIME_BASE / 1000));
    }

    return true;
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
    _flushPacketSent = false;
    _decodedFrameCursor = 0;
    _lastError.clear();
    _streamInfo = {};
  }

  bool FfmpegDecoderSession::openInput(std::filesystem::path const& filePath)
  {
    AVFormatContext* ctx = nullptr;

    std::int32_t ret = ::avformat_open_input(&ctx, filePath.string().c_str(), nullptr, nullptr);

    if (ret < 0)
    {
      setError("Failed to open input '" + filePath.string() + "': " + ffmpegErrorText(ret));
      return false;
    }

    _formatContext.reset(ctx);

    ret = ::avformat_find_stream_info(ctx, nullptr);

    if (ret < 0)
    {
      setError("Failed to read stream info: " + ffmpegErrorText(ret));
      return false;
    }

    return true;
  }

  bool FfmpegDecoderSession::openAudioStream()
  {
    if (!_formatContext)
    {
      setError("Input was not opened before opening the audio stream");
      return false;
    }

    // Find audio stream
    _audioStreamIndex = ::av_find_best_stream(_formatContext.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (_audioStreamIndex < 0)
    {
      setError("Failed to find audio stream: " + ffmpegErrorText(_audioStreamIndex));
      return false;
    }

    auto* stream = _formatContext->streams[_audioStreamIndex];

    // Find decoder
    auto const* codec = ::avcodec_find_decoder(stream->codecpar->codec_id);

    if (!codec)
    {
      setError("Failed to find decoder for the audio stream");
      return false;
    }

    // Create codec context
    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);

    if (!codecCtx)
    {
      setError("Failed to allocate codec context");
      return false;
    }

    _codecContext.reset(codecCtx);

    // Copy codec parameters to context
    std::int32_t ret = ::avcodec_parameters_to_context(_codecContext.get(), stream->codecpar);

    if (ret < 0)
    {
      setError("Failed to copy codec parameters: " + ffmpegErrorText(ret));
      return false;
    }

    // Open codec
    ret = ::avcodec_open2(_codecContext.get(), codec, nullptr);

    if (ret < 0)
    {
      setError("Failed to open codec: " + ffmpegErrorText(ret));
      return false;
    }

    // Allocate packet and frame
    _packet.reset(::av_packet_alloc());
    _frame.reset(::av_frame_alloc());

    // Store source format info
    std::int32_t rawBitDepth = stream->codecpar->bits_per_raw_sample;

    if (rawBitDepth <= 0)
    {
      rawBitDepth = _codecContext->bits_per_raw_sample;
    }

    if (rawBitDepth <= 0)
    {
      rawBitDepth = ::av_get_bytes_per_sample(_codecContext->sample_fmt) * 8;
    }

    // Determine if the source is lossy
    auto const id = stream->codecpar->codec_id;
    auto const* desc = ::avcodec_descriptor_get(id);
    bool isLossless = desc && (desc->props & AV_CODEC_PROP_LOSSLESS) != 0;

    // PCM and some other formats might not have the flag set reliably in all versions
    if (id >= AV_CODEC_ID_PCM_S16LE && id < AV_CODEC_ID_ADPCM_IMA_QT) isLossless = true;

    _streamInfo.isLossy = !isLossless;

    _streamInfo.sourceFormat.sampleRate = _codecContext->sample_rate;
    _streamInfo.sourceFormat.channels = static_cast<std::uint8_t>(_codecContext->ch_layout.nb_channels);
    _streamInfo.sourceFormat.bitDepth = static_cast<std::uint8_t>(rawBitDepth);
    _streamInfo.sourceFormat.isFloat = isFloatSampleFormat(_codecContext->sample_fmt);
    _streamInfo.sourceFormat.isInterleaved = !::av_sample_fmt_is_planar(_codecContext->sample_fmt);

    PLAYBACK_LOG_INFO("FfmpegDecoderSession: opened file, source format: {}Hz/{}b/{}ch (codec bit_depth={}, raw={})",
                      _streamInfo.sourceFormat.sampleRate,
                      _streamInfo.sourceFormat.bitDepth,
                      _streamInfo.sourceFormat.channels,
                      _codecContext->bits_per_raw_sample,
                      rawBitDepth);

    // Output format - use hint if provided, otherwise match source (resampler handles conversion)
    _streamInfo.outputFormat = _outputFormat;

    if (_outputFormat.sampleRate == 0)
    {
      _streamInfo.outputFormat.sampleRate = _streamInfo.sourceFormat.sampleRate;
    }

    if (_outputFormat.channels == 0)
    {
      _streamInfo.outputFormat.channels = _streamInfo.sourceFormat.channels;
    }

    if (_outputFormat.bitDepth == 0)
    {
      _streamInfo.outputFormat.bitDepth = _streamInfo.sourceFormat.bitDepth;
    }

    return true;
  }

  bool FfmpegDecoderSession::configureResampler()
  {
    if (!_codecContext)
    {
      setError("Codec context was not initialized before configuring the resampler");
      return false;
    }

    // Use the exact packed (interleaved) equivalent of the decoder's native format.
    // This avoids upscaling/resampling while ensuring the data is interleaved as required by the engine.
    AVSampleFormat outSampleFormat = ::av_get_packed_sample_fmt(_codecContext->sample_fmt);

    // Update our output format info to match what the resampler will actually produce.
    // This is critical for 24-bit audio which FFmpeg decodes into 32-bit (S32) containers.
    _streamInfo.outputFormat.bitDepth = static_cast<std::uint8_t>(::av_get_bytes_per_sample(outSampleFormat) * 8);
    _streamInfo.outputFormat.isFloat = isFloatSampleFormat(outSampleFormat);
    _streamInfo.outputFormat.isInterleaved = true;

    auto inChannelLayout = AVChannelLayout{};

    if (_codecContext->ch_layout.nb_channels > 0 && ::av_channel_layout_check(&_codecContext->ch_layout) != 0)
    {
      ::av_channel_layout_copy(&inChannelLayout, &_codecContext->ch_layout);
    }
    else
    {
      ::av_channel_layout_default(&inChannelLayout, _streamInfo.sourceFormat.channels);
    }

    auto outChannelLayout = AVChannelLayout{};
    ::av_channel_layout_default(&outChannelLayout, _streamInfo.outputFormat.channels);

    // Create resampler context
    SwrContext* swrCtx = ::swr_alloc();

    if (!swrCtx)
    {
      ::av_channel_layout_uninit(&inChannelLayout);
      ::av_channel_layout_uninit(&outChannelLayout);
      setError("Failed to allocate resampler");
      return false;
    }

    // Set options
    ::av_opt_set_chlayout(swrCtx, "in_chlayout", &inChannelLayout, 0);
    ::av_opt_set_int(swrCtx, "in_sample_rate", _codecContext->sample_rate, 0);
    ::av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", _codecContext->sample_fmt, 0);
    ::av_opt_set_chlayout(swrCtx, "out_chlayout", &outChannelLayout, 0);
    ::av_opt_set_int(swrCtx, "out_sample_rate", _streamInfo.outputFormat.sampleRate, 0);
    ::av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", outSampleFormat, 0);

    // Initialize
    std::int32_t ret = ::swr_init(swrCtx);
    ::av_channel_layout_uninit(&inChannelLayout);
    ::av_channel_layout_uninit(&outChannelLayout);

    if (ret < 0)
    {
      setError("Failed to initialize resampler: " + ffmpegErrorText(ret));
      ::swr_free(&swrCtx);
      return false;
    }

    _swrContext.reset(swrCtx);
    return true;
  }

  bool FfmpegDecoderSession::seek(std::uint32_t positionMs)
  {
    if (!_formatContext || _audioStreamIndex < 0)
    {
      return false;
    }

    auto const* stream = _formatContext->streams[_audioStreamIndex];
    auto const timestamp = ::av_rescale_q(static_cast<std::int64_t>(positionMs), {1, 1000}, stream->time_base);
    auto const seekFlags = positionMs > getCurrentPositionMs() ? 0 : AVSEEK_FLAG_BACKWARD;

    std::int32_t ret = ::av_seek_frame(_formatContext.get(), _audioStreamIndex, timestamp, seekFlags);

    if (ret < 0)
    {
      setError("Seek failed at " + std::to_string(positionMs) + " ms: " + ffmpegErrorText(ret));
      return false;
    }

    // Flush decoder
    flush();

    if (_streamInfo.outputFormat.sampleRate > 0)
    {
      _decodedFrameCursor = (static_cast<std::uint64_t>(positionMs) * _streamInfo.outputFormat.sampleRate) / 1000;
    }

    return true;
  }

  void FfmpegDecoderSession::flush()
  {
    if (_codecContext)
    {
      ::avcodec_flush_buffers(_codecContext.get());
    }

    if (_swrContext)
    {
      ::swr_close(_swrContext.get());
      ::swr_init(_swrContext.get());
    }

    _decoderEof = false;
    _inputEof = false;
    _flushPacketSent = false;
  }

  std::uint32_t FfmpegDecoderSession::getCurrentPositionMs() const
  {
    if (!_formatContext || !_codecContext || !_frame)
    {
      return 0;
    }

    auto const stream = _formatContext->streams[_audioStreamIndex];

    if (!stream)
    {
      return 0;
    }

    auto const base = stream->time_base;
    auto const pts = _frame->pts;

    if (pts == AV_NOPTS_VALUE)
    {
      return 0;
    }

    return static_cast<std::uint32_t>(::av_rescale_q(pts, base, {1, 1000}));
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
      auto block = PcmBlock{};
      block.endOfStream = true;
      return block;
    }

    while (true)
    {
      ::av_frame_unref(_frame.get());
      std::int32_t ret = ::avcodec_receive_frame(_codecContext.get(), _frame.get());

      if (ret == 0)
      {

        if (auto block = convertFrameToInterleavedPcm(); block && block->frames > 0)
        {
          return block;
        }

        continue;
      }

      if (ret == AVERROR_EOF)
      {
        _decoderEof = true;
        auto block = PcmBlock{};
        block.endOfStream = true;
        return block;
      }

      if (ret != AVERROR(EAGAIN))
      {
        setError("Error receiving decoded audio frame: " + ffmpegErrorText(ret));
        return std::nullopt;
      }

      if (_inputEof)
      {
        if (_flushPacketSent)
        {
          _decoderEof = true;
          auto block = PcmBlock{};
          block.endOfStream = true;
          return block;
        }

        ret = ::avcodec_send_packet(_codecContext.get(), nullptr);

        if (ret < 0 && ret != AVERROR(EAGAIN))
        {
          setError("Error flushing decoder at end of stream: " + ffmpegErrorText(ret));
          return std::nullopt;
        }

        _flushPacketSent = true;
        continue;
      }

      ::av_packet_unref(_packet.get());
      ret = ::av_read_frame(_formatContext.get(), _packet.get());

      if (ret == AVERROR_EOF)
      {
        _inputEof = true;
        continue;
      }

      if (ret < 0)
      {
        setError("Error reading compressed audio packet: " + ffmpegErrorText(ret));
        return std::nullopt;
      }

      if (_packet->stream_index != _audioStreamIndex)
      {
        ::av_packet_unref(_packet.get());
        continue;
      }

      ret = ::avcodec_send_packet(_codecContext.get(), _packet.get());
      ::av_packet_unref(_packet.get());

      if (ret < 0 && ret != AVERROR(EAGAIN))
      {
        setError("Error sending compressed audio packet to decoder: " + ffmpegErrorText(ret));
        return std::nullopt;
      }
    }
  }

  std::optional<PcmBlock> FfmpegDecoderSession::convertFrameToInterleavedPcm()
  {
    if (!_frame || !_swrContext)
    {
      return std::nullopt;
    }

    auto const outChannels = _streamInfo.outputFormat.channels;
    auto const outBitDepth = _streamInfo.outputFormat.bitDepth;

    // Calculate output buffer size in samples
    auto const outSamples =
      ::av_rescale_rnd(::swr_get_delay(_swrContext.get(), _frame->sample_rate) + _frame->nb_samples,
                       _streamInfo.outputFormat.sampleRate,
                       _frame->sample_rate,
                       ::AVRounding::AV_ROUND_UP);

    if (outSamples <= 0)
    {
      return std::nullopt;
    }

    auto block = PcmBlock{};
    block.bitDepth = outBitDepth;
    block.firstFrameIndex = _decodedFrameCursor;
    block.endOfStream = false;

    std::int32_t convertedSamples = 0;

    if (outBitDepth == 32)
    {
      // 32-bit output stays in the native S32 container from swr.
      std::vector<std::int32_t> outBuffer(outSamples * outChannels);
      auto* outPtr = reinterpret_cast<std::uint8_t*>(outBuffer.data());
      convertedSamples = ::swr_convert(_swrContext.get(),
                                       &outPtr,
                                       static_cast<int>(outSamples),
                                       const_cast<std::uint8_t const**>(_frame->data),
                                       _frame->nb_samples);

      if (convertedSamples <= 0)
      {
        return std::nullopt;
      }

      block.frames = static_cast<std::uint32_t>(convertedSamples);
      auto const byteCount = static_cast<std::size_t>(convertedSamples) * outChannels * sizeof(std::int32_t);
      block.bytes.resize(byteCount);
      ::memcpy(block.bytes.data(), outBuffer.data(), byteCount);
    }
    else if (outBitDepth == 24)
    {
      // swr emits hi-res integer PCM as S32; pack it down to little-endian S24.
      std::vector<std::int32_t> outBuffer(outSamples * outChannels);
      auto* outPtr = reinterpret_cast<std::uint8_t*>(outBuffer.data());
      convertedSamples = ::swr_convert(_swrContext.get(),
                                       &outPtr,
                                       static_cast<int>(outSamples),
                                       const_cast<std::uint8_t const**>(_frame->data),
                                       _frame->nb_samples);

      if (convertedSamples <= 0)
      {
        return std::nullopt;
      }

      block.frames = static_cast<std::uint32_t>(convertedSamples);
      auto const sampleCount = static_cast<std::size_t>(convertedSamples) * outChannels;
      block.bytes.resize(sampleCount * 3);

      for (std::size_t i = 0; i < sampleCount; ++i)
      {
        auto const sample = static_cast<std::uint32_t>(outBuffer[i]);
        block.bytes[i * 3] = static_cast<std::byte>((sample >> 8) & 0xFFu);
        block.bytes[i * 3 + 1] = static_cast<std::byte>((sample >> 16) & 0xFFu);
        block.bytes[i * 3 + 2] = static_cast<std::byte>((sample >> 24) & 0xFFu);
      }
    }
    else
    {
      // 16-bit: output as S16 from swr
      std::vector<std::int16_t> outBuffer(outSamples * outChannels);
      auto* outPtr = reinterpret_cast<std::uint8_t*>(outBuffer.data());
      convertedSamples = ::swr_convert(_swrContext.get(),
                                       &outPtr,
                                       static_cast<int>(outSamples),
                                       const_cast<std::uint8_t const**>(_frame->data),
                                       _frame->nb_samples);

      if (convertedSamples <= 0)
      {
        return std::nullopt;
      }

      block.frames = static_cast<std::uint32_t>(convertedSamples);
      // Convert to bytes
      auto const byteCount = convertedSamples * outChannels * sizeof(std::int16_t);
      block.bytes.resize(byteCount);
      ::memcpy(block.bytes.data(), outBuffer.data(), byteCount);
    }

    // Update cursor (estimate based on sample rate ratio)
    auto const durationRatio = static_cast<double>(_streamInfo.outputFormat.sampleRate) / _frame->sample_rate;
    _decodedFrameCursor += static_cast<std::uint64_t>(_frame->nb_samples * durationRatio);

    return block;
  }

  DecodedStreamInfo FfmpegDecoderSession::streamInfo() const
  {
    return _streamInfo;
  }

  std::string_view FfmpegDecoderSession::lastError() const noexcept
  {
    return _lastError;
  }

  void FfmpegDecoderSession::setError(std::string message)
  {
    PLAYBACK_LOG_ERROR("FFmpeg error: {}", message);
    _lastError = std::move(message);
  }

} // namespace app::core::playback

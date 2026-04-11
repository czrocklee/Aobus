// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "PipeWireBackend.h"

extern "C"
{
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>
#include <spa/utils/type.h>
}

#include <cstring>
#include <iostream>

namespace app::playback
{

  static void onStreamProcess(void* data)
  {
    auto* self = static_cast<PipeWireBackend*>(data);
    self->process();
  }

  static const struct pw_stream_events streamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = onStreamProcess,
  };

  PipeWireBackend::PipeWireBackend()
  {
    pw_init(nullptr, nullptr);
  }

  PipeWireBackend::~PipeWireBackend()
  {
    stop();
    if (_stream)
    {
      pw_stream_destroy(_stream);
      _stream = nullptr;
    }
    if (_context)
    {
      pw_context_destroy(_context);
      _context = nullptr;
    }
    if (_threadLoop)
    {
      pw_thread_loop_stop(_threadLoop);
      pw_thread_loop_destroy(_threadLoop);
      _threadLoop = nullptr;
    }
  }

  void PipeWireBackend::open(StreamFormat const& format, AudioRenderCallbacks callbacks)
  {
    _callbacks = callbacks;
    _format = format;

    // Create thread loop
    _threadLoop = pw_thread_loop_new("rockstudio-pw", nullptr);
    if (!_threadLoop)
    {
      std::cerr << "Failed to create PipeWire thread loop" << std::endl;
      return;
    }

    // Create context
    _context = pw_context_new(pw_thread_loop_get_loop(_threadLoop), nullptr, 0);
    if (!_context)
    {
      std::cerr << "Failed to create PipeWire context" << std::endl;
      return;
    }

    // Start thread loop
    if (pw_thread_loop_start(_threadLoop) < 0)
    {
      std::cerr << "Failed to start PipeWire thread loop" << std::endl;
      return;
    }

    // Lock and create stream
    pw_thread_loop_lock(_threadLoop);

    // Stream properties
    pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_APP_NAME, "RockStudio", nullptr);

    _stream = pw_stream_new_simple(pw_thread_loop_get_loop(_threadLoop), "playback", props, &streamEvents, this);

    if (!_stream)
    {
      std::cerr << "Failed to create PipeWire stream" << std::endl;
      pw_thread_loop_unlock(_threadLoop);
      return;
    }

    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::start()
  {
    if (!_stream || !_threadLoop)
    {
      return;
    }

    pw_thread_loop_lock(_threadLoop);

    // Build format params manually
    uint8_t buffer[1024];
    struct spa_pod_builder builder;
    spa_pod_builder_init(&builder, buffer, sizeof(buffer));

    // Use spa_pod_builder_add with raw POD building
    // Build in-place using spa_pod_builder
    spa_pod_builder_init(&builder, buffer, sizeof(buffer));

    // Push object start
    struct spa_pod_frame frame;
    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);

    // Determine SPA audio format based on bit depth
    int spaFormat = SPA_AUDIO_FORMAT_S16_LE;
    if (_format.bitDepth == 24)
    {
      spaFormat = SPA_AUDIO_FORMAT_S24_LE;
    }
    else if (_format.bitDepth == 32)
    {
      spaFormat = SPA_AUDIO_FORMAT_S32_LE;
    }

    // Add properties
    spa_pod_builder_add(&builder,
                        SPA_FORMAT_mediaType,
                        SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                        SPA_FORMAT_mediaSubtype,
                        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_AUDIO_format,
                        SPA_POD_Id(spaFormat),
                        SPA_FORMAT_AUDIO_rate,
                        SPA_POD_Int(_format.sampleRate),
                        SPA_FORMAT_AUDIO_channels,
                        SPA_POD_Int(_format.channels),
                        0);

    spa_pod_builder_pop(&builder, &frame);

    // Get the built pod
    struct spa_pod* param = reinterpret_cast<struct spa_pod*>(buffer);
    const struct spa_pod* constParams = param;

    // Connect stream to remote (default target)
    int ret = pw_stream_connect(_stream,
                                PW_DIRECTION_OUTPUT,
                                PW_ID_ANY,
                                static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                                &constParams,
                                1);

    if (ret < 0)
    {
      std::cerr << "Failed to connect PipeWire stream: " << ret << std::endl;
    }

    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::pause()
  {
    if (!_stream || !_threadLoop)
    {
      return;
    }

    pw_thread_loop_lock(_threadLoop);
    pw_stream_set_active(_stream, false);
    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::resume()
  {
    if (!_stream || !_threadLoop)
    {
      return;
    }

    pw_thread_loop_lock(_threadLoop);
    pw_stream_set_active(_stream, true);
    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::flush()
  {
    // Flush is handled via the ring buffer in the engine
  }

  void PipeWireBackend::stop()
  {
    if (!_stream || !_threadLoop)
    {
      return;
    }

    pw_thread_loop_lock(_threadLoop);
    pw_stream_set_active(_stream, false);
    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::process()
  {
    if (!_callbacks.readPcm)
    {
      return;
    }

    // Get buffer from PipeWire
    struct pw_buffer* buffer = pw_stream_dequeue_buffer(_stream);
    if (!buffer)
    {
      if (_callbacks.onUnderrun)
      {
        _callbacks.onUnderrun(_callbacks.userData);
      }
      return;
    }

    auto* data = static_cast<std::byte*>(buffer->buffer->datas[0].data);
    auto const size = buffer->buffer->datas[0].maxsize;
    auto const bytesPerSample = (_format.bitDepth == 24) ? 3u : (_format.bitDepth == 32) ? 4u : 2u;
    auto const frameBytes = static_cast<std::size_t>(_format.channels) * bytesPerSample;
    auto const requestSize = frameBytes > 0 ? size - (size % frameBytes) : 0;

    if (data && requestSize > 0)
    {
      std::span<std::byte> output(data, requestSize);
      auto const read = _callbacks.readPcm(_callbacks.userData, output);
      auto const alignedRead = read - (read % frameBytes);
      std::cerr << "[DEBUG] PW process: requested=" << requestSize << " read=" << alignedRead << std::endl;

      buffer->buffer->datas[0].chunk->offset = 0;
      buffer->buffer->datas[0].chunk->size = static_cast<uint32_t>(alignedRead);
      buffer->buffer->datas[0].chunk->stride = static_cast<int32_t>(frameBytes);
    }

    pw_stream_queue_buffer(_stream, buffer);
  }

} // namespace app::playback

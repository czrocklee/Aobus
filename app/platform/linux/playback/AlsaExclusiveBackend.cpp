// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/AlsaExclusiveBackend.h"
#include "core/Log.h"
#include "core/util/ThreadUtils.h"
#include "core/backend/BackendTypes.h"

#include <rs/utility/Raii.h>
#include <poll.h>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

namespace app::playback
{
  AlsaExclusiveBackend::AlsaExclusiveBackend(app::core::backend::AudioDevice const& device)
    : _deviceName{device.id}
  {
    PLAYBACK_LOG_DEBUG("AlsaExclusiveBackend: Creating backend instance for device '{}'", _deviceName);
  }

  AlsaExclusiveBackend::~AlsaExclusiveBackend()
  {
    PLAYBACK_LOG_DEBUG("AlsaExclusiveBackend: Destroying backend instance");
    stop();
    close();
  }

  bool AlsaExclusiveBackend::open(app::core::AudioFormat const& format, app::core::backend::AudioRenderCallbacks callbacks)
  {
    PLAYBACK_LOG_INFO("AlsaExclusiveBackend: Opening device '{}' with format {}Hz/{}b/{}ch",
                      _deviceName, format.sampleRate, (int)format.bitDepth, (int)format.channels);
    close();
    _callbacks = callbacks;
    _lastError.clear();

    ::snd_pcm_t* pcm = nullptr;
    if (::snd_pcm_open(&pcm, _deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, 0) < 0) {
      _lastError = "Failed to open ALSA device: " + _deviceName;
      PLAYBACK_LOG_ERROR("{}", _lastError);
      return false;
    }

    auto safePcm = AlsaPcmPtr(pcm);
    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    if (::snd_pcm_hw_params_any(safePcm.get(), params) < 0) { _lastError = "Failed to init ALSA hw params"; return false; }
    if (::snd_pcm_hw_params_set_access(safePcm.get(), params, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0) { _lastError = "No mmap interleaved support"; return false; }

    auto alsaFormat = SND_PCM_FORMAT_S16_LE;
    if (format.bitDepth == 32) alsaFormat = (format.validBits == 24) ? SND_PCM_FORMAT_S24_LE : SND_PCM_FORMAT_S32_LE;
    else if (format.bitDepth == 24) alsaFormat = SND_PCM_FORMAT_S24_3LE;

    if (::snd_pcm_hw_params_set_format(safePcm.get(), params, alsaFormat) < 0) { _lastError = "Bit depth not supported"; return false; }
    unsigned int rate = format.sampleRate;
    if (::snd_pcm_hw_params_set_rate_near(safePcm.get(), params, &rate, 0) < 0) { _lastError = "Failed to set rate"; return false; }
    if (::snd_pcm_hw_params_set_channels(safePcm.get(), params, format.channels) < 0) { _lastError = "Failed to set channels"; return false; }

    unsigned int periods = 4;
    snd_pcm_uframes_t periodSize = 1024;
    ::snd_pcm_hw_params_set_periods_near(safePcm.get(), params, &periods, 0);
    ::snd_pcm_hw_params_set_period_size_near(safePcm.get(), params, &periodSize, 0);
    if (::snd_pcm_hw_params(safePcm.get(), params) < 0) { _lastError = "Failed to apply hw params"; return false; }

    ::snd_pcm_sw_params_t* swParams = nullptr;
    snd_pcm_sw_params_alloca(&swParams);
    if (::snd_pcm_sw_params_current(safePcm.get(), swParams) < 0) { _lastError = "Failed to get sw params"; return false; }
    ::snd_pcm_sw_params_set_start_threshold(safePcm.get(), swParams, periodSize);
    ::snd_pcm_sw_params_set_avail_min(safePcm.get(), swParams, periodSize);
    if (::snd_pcm_sw_params(safePcm.get(), swParams) < 0) { _lastError = "Failed to apply sw params"; return false; }

    _format = format;
    _format.sampleRate = rate;
    _pcm = std::move(safePcm);

    if (_callbacks.onGraphChanged) {
      app::core::backend::AudioGraph graph;
      graph.nodes.push_back({.id = "alsa-stream", .type = app::core::backend::AudioNodeType::Stream, .name = "ALSA Stream", .format = _format});
      graph.nodes.push_back({.id = "alsa-sink", .type = app::core::backend::AudioNodeType::Sink, .name = _deviceName, .format = _format, .objectPath = _deviceName});
      graph.links.push_back({.sourceId = "alsa-stream", .destId = "alsa-sink", .isActive = true});
      _callbacks.onGraphChanged(_callbacks.userData, graph);
    }
    return true;
  }

  void AlsaExclusiveBackend::playbackLoop(std::stop_token stopToken)
  {
    while (!stopToken.stop_requested()) {
      if (_paused.load(std::memory_order_relaxed)) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
      if (::snd_pcm_wait(_pcm.get(), 500) < 0) { recoverFromXrun(-EPIPE); continue; }
      ::snd_pcm_sframes_t avail = ::snd_pcm_avail_update(_pcm.get());
      if (avail < 0) { recoverFromXrun(static_cast<int>(avail)); continue; }
      if (avail == 0) continue;

      ::snd_pcm_uframes_t frames = static_cast<::snd_pcm_uframes_t>(avail);
      ::snd_pcm_channel_area_t const* areas = nullptr;
      ::snd_pcm_uframes_t offset = 0;
      if (::snd_pcm_mmap_begin(_pcm.get(), &areas, &offset, &frames) < 0) { recoverFromXrun(-EPIPE); continue; }

      char* dest = static_cast<char*>(areas[0].addr) + (offset * (areas[0].step / 8));
      std::size_t const bytesToRead = frames * (_format.bitDepth / 8) * _format.channels;
      std::size_t const bytesRead = _callbacks.readPcm(_callbacks.userData, {reinterpret_cast<std::byte*>(dest), bytesToRead});

      if (bytesRead > 0) {
        auto committed = ::snd_pcm_mmap_commit(_pcm.get(), offset, bytesRead / ((_format.bitDepth / 8) * _format.channels));
        if (committed < 0) recoverFromXrun(static_cast<int>(committed));
        else if (_callbacks.onPositionAdvanced) _callbacks.onPositionAdvanced(_callbacks.userData, static_cast<std::uint32_t>(committed));
      } else {
        ::snd_pcm_mmap_commit(_pcm.get(), offset, 0);
        if (_callbacks.isSourceDrained(_callbacks.userData)) {
          ::snd_pcm_drain(_pcm.get());
          if (_callbacks.onDrainComplete) _callbacks.onDrainComplete(_callbacks.userData);
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }

  void AlsaExclusiveBackend::recoverFromXrun(int err)
  {
    if (err == -EPIPE) { if (_callbacks.onUnderrun) _callbacks.onUnderrun(_callbacks.userData); ::snd_pcm_prepare(_pcm.get()); }
    else if (err == -ESTRPIPE) { while (::snd_pcm_resume(_pcm.get()) == -EAGAIN) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    else if (err == -ENODEV || err == -EBADF) {
      _lastError = "ALSA Device Lost: " + std::string(::snd_strerror(err));
      if (_callbacks.onBackendError) _callbacks.onBackendError(_callbacks.userData, _lastError);
    }
  }

  void AlsaExclusiveBackend::start()
  {
    if (!_pcm) return;
    _paused = false;
    if (!_thread.joinable()) _thread = std::jthread([this](std::stop_token st) { app::core::util::setCurrentThreadName("AlsaPlayback"); playbackLoop(st); });
    ::snd_pcm_start(_pcm.get());
  }

  void AlsaExclusiveBackend::pause() { if (_pcm) { _paused = true; ::snd_pcm_pause(_pcm.get(), 1); } }
  void AlsaExclusiveBackend::resume() { if (_pcm) { _paused = false; ::snd_pcm_pause(_pcm.get(), 0); } }
  void AlsaExclusiveBackend::flush() { if (_pcm) { ::snd_pcm_drop(_pcm.get()); ::snd_pcm_prepare(_pcm.get()); } }
  void AlsaExclusiveBackend::drain() { if (!_pcm) { if (_callbacks.onDrainComplete) _callbacks.onDrainComplete(_callbacks.userData); return; } ::snd_pcm_drain(_pcm.get()); if (_callbacks.onDrainComplete) _callbacks.onDrainComplete(_callbacks.userData); }
  void AlsaExclusiveBackend::stop() { _thread.request_stop(); if (_thread.joinable() && std::this_thread::get_id() != _thread.get_id()) _thread.join(); if (_pcm) { ::snd_pcm_drop(_pcm.get()); ::snd_pcm_prepare(_pcm.get()); } }
  void AlsaExclusiveBackend::close() { stop(); _pcm.reset(); }
  void AlsaExclusiveBackend::setExclusiveMode(bool) {}
  bool AlsaExclusiveBackend::isExclusiveMode() const noexcept { return true; }
  app::core::backend::BackendKind AlsaExclusiveBackend::kind() const noexcept { return app::core::backend::BackendKind::AlsaExclusive; }
  std::string_view AlsaExclusiveBackend::lastError() const noexcept { return _lastError; }
} // namespace app::playback

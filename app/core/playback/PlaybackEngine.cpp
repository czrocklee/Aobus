// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/PlaybackEngine.h"
#include "core/Log.h"

#include "core/decoder/AudioDecoderFactory.h"
#include "core/decoder/IAudioDecoderSession.h"
#include "core/source/MemoryPcmSource.h"
#include "core/source/StreamingPcmSource.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <limits>
#include <set>
#include <sstream>

namespace app::core::playback
{

  namespace
  {
    constexpr std::uint32_t kPrerollTargetMs = 200;
    constexpr std::uint32_t kDecodeHighWatermarkMs = 750;
    constexpr std::uint64_t kMemoryPcmSourceBudgetBytes = 64ULL * 1024ULL * 1024ULL;

    bool isLosslessBitDepthChange(AudioFormat const& src, AudioFormat const& dst) noexcept
    {
      if (src.isFloat == dst.isFloat)
      {
        return src.bitDepth <= dst.bitDepth;
      }

      if (!src.isFloat && dst.isFloat)
      {
        if (dst.bitDepth == 32) return src.bitDepth <= 24;
        if (dst.bitDepth == 64) return src.bitDepth <= 32;
      }

      return false;
    }

    void appendLine(std::string& text, std::string_view line)
    {
      if (line.empty()) return;
      if (!text.empty()) text += '\n';
      text += line;
    }

    std::uint64_t bytesPerSecond(AudioFormat const& format) noexcept
    {
      if (format.sampleRate == 0 || format.channels == 0 || format.bitDepth == 0)
      {
        return 0;
      }

      auto const bytesPerSample = (format.bitDepth == 24U) ? 3U : (format.bitDepth > 16U) ? 4U : 2U;
      return static_cast<std::uint64_t>(format.sampleRate) * format.channels * bytesPerSample;
    }

    std::uint64_t estimatedDecodedBytes(app::core::decoder::DecodedStreamInfo const& info) noexcept
    {
      auto const rate = bytesPerSecond(info.outputFormat);
      if (rate == 0 || info.durationMs == 0) return 0;
      return (static_cast<std::uint64_t>(info.durationMs) * rate) / 1000U;
    }

    bool shouldUseMemoryPcmSource(app::core::decoder::DecodedStreamInfo const& info) noexcept
    {
      auto const decodedBytes = estimatedDecodedBytes(info);
      return decodedBytes > 0 && decodedBytes <= kMemoryPcmSourceBudgetBytes;
    }
  } // namespace
  using namespace app::core::backend;
  using namespace app::core::decoder;

  PlaybackEngine::PlaybackEngine(std::unique_ptr<backend::IAudioBackend> backend)
    : _backend{std::move(backend)}
  {
    _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
  }

  PlaybackEngine::~PlaybackEngine()
  {
    stop();
  }

  void PlaybackEngine::setBackend(std::unique_ptr<backend::IAudioBackend> backend, std::string deviceId)
  {
    struct State
    {
      std::optional<TrackPlaybackDescriptor> track;
      std::uint32_t positionMs = 0;
      bool wasPlaying = false;
    };

    auto const state = [this]()
    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      return State{
        .track = _currentTrack, .positionMs = _snapshot.positionMs, .wasPlaying = (_state == TransportState::Playing)};
    }();

    stop();

    _backend = std::move(backend);

    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
      _snapshot.currentDeviceId = std::move(deviceId);
    }

    if (state.track)
    {
      PLAYBACK_LOG_INFO("Resuming track '{}' after backend switch", state.track->title);
      play(*state.track);
      seek(state.positionMs);
      if (!state.wasPlaying)
      {
        pause();
      }
    }
  }

  void PlaybackEngine::play(TrackPlaybackDescriptor descriptor)
  {
    PLAYBACK_LOG_INFO(
      "Play requested: {} - {} [{}]", descriptor.artist, descriptor.title, descriptor.filePath.string());

    if (_backend)
    {
      _backend->stop();
      _backend->close();
    }

    _source.store({}, std::memory_order_release);

    auto callbacks = backend::AudioRenderCallbacks{};
    callbacks.userData = this;
    callbacks.readPcm = &PlaybackEngine::onReadPcm;
    callbacks.isSourceDrained = &PlaybackEngine::isSourceDrained;
    callbacks.onUnderrun = &PlaybackEngine::onUnderrun;
    callbacks.onPositionAdvanced = &PlaybackEngine::onPositionAdvanced;
    callbacks.onDrainComplete = &PlaybackEngine::onDrainComplete;
    callbacks.onGraphChanged = &PlaybackEngine::onGraphChanged;
    callbacks.onBackendError = &PlaybackEngine::onBackendError;

    auto source = std::shared_ptr<source::IPcmSource>{};
    auto backendFormat = AudioFormat{};

    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _underrunCount = 0;
      _backendStarted = false;
      _playbackDrainPending = false;
      _snapshot = {};
      _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
      _snapshot.state = TransportState::Opening;
      _snapshot.trackTitle = descriptor.title;
      _snapshot.trackArtist = descriptor.artist;
      _currentTrack = descriptor;

      if (!openTrack(descriptor, source, backendFormat))
      {
        _state = TransportState::Error;
        _snapshot.state = TransportState::Error;
        _currentTrack.reset();
        return;
      }

      _state = TransportState::Buffering;
      _snapshot.state = TransportState::Buffering;
    }

    _source.store(source, std::memory_order_release);

    if (_backend && !_backend->open(backendFormat, callbacks))
    {
      _source.store({}, std::memory_order_release);
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _currentTrack.reset();
      _state = TransportState::Error;
      _snapshot.state = TransportState::Error;
      _snapshot.statusText = std::string(_backend->lastError());
      return;
    }

    auto const bufferedMs = source ? source->bufferedMs() : 0;
    if (auto const drained = !source || source->isDrained(); drained && bufferedMs == 0)
    {
      if (_backend)
      {
        _backend->stop();
        _backend->close();
      }
      _source.store({}, std::memory_order_release);
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _currentTrack.reset();
      _backendStarted = false;
      _playbackDrainPending = false;
      _state = TransportState::Idle;
      _snapshot = {};
      _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
      return;
    }

    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _state = TransportState::Playing;
      _snapshot.state = TransportState::Playing;
      _backendStarted = true;
    }

    if (_backend) _backend->start();
  }

  void PlaybackEngine::pause()
  {
    auto lock = std::lock_guard<std::mutex>{_stateMutex};
    if (_state == TransportState::Playing || _state == TransportState::Buffering)
    {
      PLAYBACK_LOG_INFO("Playback paused");
      _state = TransportState::Paused;
      _snapshot.state = TransportState::Paused;
      if (_backend && _backendStarted) _backend->pause();
    }
  }

  void PlaybackEngine::resume()
  {
    auto source = _source.load(std::memory_order_acquire);
    auto lock = std::unique_lock<std::mutex>{_stateMutex};
    if (_state != TransportState::Paused) return;

    PLAYBACK_LOG_INFO("Playback resumed");
    if (_backendStarted)
    {
      _state = TransportState::Playing;
      _snapshot.state = TransportState::Playing;
      lock.unlock();
      if (_backend) _backend->resume();
      return;
    }

    auto const bufferedMs = source ? source->bufferedMs() : 0;
    auto const drained = !source || source->isDrained();
    if (drained && bufferedMs == 0)
    {
      _source.store({}, std::memory_order_release);
      _currentTrack.reset();
      _state = TransportState::Idle;
      _snapshot = {};
      _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
      return;
    }

    _state = TransportState::Playing;
    _snapshot.state = TransportState::Playing;
    _backendStarted = true;
    lock.unlock();
    if (_backend) _backend->start();
  }

  void PlaybackEngine::stop()
  {
    PLAYBACK_LOG_INFO("Playback stopped");
    if (_backend)
    {
      _backend->stop();
      _backend->close();
    }
    _source.store({}, std::memory_order_release);
    auto lock = std::lock_guard<std::mutex>{_stateMutex};
    _currentTrack.reset();
    _backendStarted = false;
    _playbackDrainPending = false;
    _state = TransportState::Idle;
    _snapshot = {};
    _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
  }

  void PlaybackEngine::seek(std::uint32_t positionMs)
  {
    PLAYBACK_LOG_INFO("Seek requested: {} ms", positionMs);
    auto source = _source.load(std::memory_order_acquire);
    if (!source) return;

    bool wasPaused = false;
    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      wasPaused = (_state == TransportState::Paused);
      _state = TransportState::Buffering;
      _snapshot.state = TransportState::Buffering;
      _snapshot.positionMs = positionMs;
      _snapshot.statusText.clear();
    }

    if (_backend)
    {
      _backend->stop();
      _backend->flush();
    }
    _backendStarted = false;
    _playbackDrainPending = false;

    if (!source->seek(positionMs))
    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _state = TransportState::Error;
      _snapshot.state = TransportState::Error;
      _snapshot.statusText = source->lastError();
      return;
    }

    auto const bufferedMs = source->bufferedMs();
    auto const drained = source->isDrained();
    if (drained && bufferedMs == 0)
    {
      if (_backend)
      {
        _backend->stop();
        _backend->close();
      }
      _source.store({}, std::memory_order_release);
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _currentTrack.reset();
      _state = TransportState::Idle;
      _snapshot = {};
      _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
      return;
    }

    if (wasPaused)
    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _state = TransportState::Paused;
      _snapshot.state = TransportState::Paused;
      return;
    }

    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _state = TransportState::Playing;
      _snapshot.state = TransportState::Playing;
      _backendStarted = true;
    }

    if (_backend) _backend->start();
  }

  PlaybackSnapshot PlaybackEngine::snapshot() const
  {
    auto source = _source.load(std::memory_order_acquire);
    auto lock = std::lock_guard<std::mutex>{_stateMutex};
    auto snap = _snapshot;
    snap.backend = _backend ? _backend->kind() : BackendKind::None;
    snap.bufferedMs = source ? source->bufferedMs() : 0;
    snap.underrunCount = _underrunCount.load(std::memory_order_relaxed);

    return snap;
  }

  void PlaybackEngine::onBackendError(void* userData, std::string_view message) noexcept
  {
    static_cast<PlaybackEngine*>(userData)->handleBackendError(message);
  }

  void PlaybackEngine::handleBackendError(std::string_view message)
  {
    PLAYBACK_LOG_ERROR("Backend error: {}", message);

    // Stop immediately
    stop();

    auto lock = std::lock_guard<std::mutex>{_stateMutex};
    _state = TransportState::Error;
    _snapshot.state = TransportState::Error;
    _snapshot.statusText = std::string(message);
  }

  bool PlaybackEngine::openTrack(TrackPlaybackDescriptor descriptor,
                                 std::shared_ptr<source::IPcmSource>& source,
                                 AudioFormat& backendFormat)
  {
    auto outputFormat = AudioFormat{};
    outputFormat.sampleRate = 0; // Use native
    outputFormat.channels = 0;   // Use native
    outputFormat.bitDepth = 0;   // Use native
    outputFormat.isFloat = false;
    outputFormat.isInterleaved = true;

    auto decoder = createAudioDecoderSession(descriptor.filePath, outputFormat);
    if (!decoder)
    {
      _snapshot.statusText = "No audio decoder backend is available";
      return false;
    }
    if (!decoder->open(descriptor.filePath))
    {
      _snapshot.statusText = std::string(decoder->lastError());
      return false;
    }

    auto const info = decoder->streamInfo();
    if (info.outputFormat.sampleRate == 0 || info.outputFormat.channels == 0 || info.outputFormat.bitDepth == 0)
    {
      _snapshot.statusText = "Decoder did not return a valid output format";
      return false;
    }

    if (shouldUseMemoryPcmSource(info))
    {
      auto memorySource = std::make_shared<source::MemoryPcmSource>(std::move(decoder), info);
      if (!memorySource->initialize())
      {
        _snapshot.statusText = memorySource->lastError();
        return false;
      }
      source = std::move(memorySource);
    }
    else
    {
      source::PcmSourceCallbacks sourceCallbacks;
      sourceCallbacks.userData = this;
      sourceCallbacks.onError = &PlaybackEngine::onSourceError;
      auto streamingSource = std::make_shared<source::StreamingPcmSource>(
        std::move(decoder), info, sourceCallbacks, kPrerollTargetMs, kDecodeHighWatermarkMs);
      if (!streamingSource->initialize())
      {
        _snapshot.statusText = streamingSource->lastError();
        return false;
      }
      source = std::move(streamingSource);
    }

    _snapshot.durationMs = info.durationMs;
    _snapshot.positionMs = 0;
    _snapshot.graph = {};

    // Add initial Source (Decoder) Node
    _snapshot.graph.nodes.push_back({.id = "rs-decoder",
                                     .type = AudioNodeType::Decoder,
                                     .name = "File Decoder",
                                     .format = info.sourceFormat,
                                     .isLossySource = info.isLossy,
                                     .objectPath = ""});

    // Add Engine Node
    _snapshot.graph.nodes.push_back({.id = "rs-engine",
                                     .type = AudioNodeType::Engine,
                                     .name = "RockStudio Engine",
                                     .format = info.outputFormat,
                                     .objectPath = ""});

    _snapshot.graph.links.push_back({.sourceId = "rs-decoder", .destId = "rs-engine", .isActive = true});

    backendFormat = info.outputFormat;
    return true;
  }

  void PlaybackEngine::handleGraphChanged(backend::AudioGraph const& backendGraph)
  {
    auto lock = std::lock_guard<std::mutex>{_stateMutex};

    PLAYBACK_LOG_DEBUG(
      "Graph update received from backend ({} nodes, {} links)", backendGraph.nodes.size(), backendGraph.links.size());

    // Keep our internal nodes (Decoder, Engine)
    auto nodes = std::vector<AudioNode>{};
    auto links = std::vector<AudioLink>{};

    for (auto const& node : _snapshot.graph.nodes)
    {
      if (node.type == AudioNodeType::Decoder || node.type == AudioNodeType::Engine) nodes.push_back(node);
    }
    for (auto const& link : _snapshot.graph.links)
    {
      if (link.sourceId == "rs-decoder" && link.destId == "rs-engine") links.push_back(link);
    }

    // Add backend nodes
    for (auto const& node : backendGraph.nodes) nodes.push_back(node);
    for (auto const& link : backendGraph.links) links.push_back(link);

    // Bridge our Engine to the Backend Stream
    bool bridged = false;
    for (auto const& node : backendGraph.nodes)
    {
      if (node.type == AudioNodeType::Stream)
      {
        links.push_back({.sourceId = "rs-engine", .destId = node.id, .isActive = true});
        PLAYBACK_LOG_DEBUG("Bridged Engine to backend stream '{}'", node.id);
        bridged = true;
        break;
      }
    }

    if (!bridged)
    {
      PLAYBACK_LOG_WARN("Could not find backend stream node to bridge the engine!");
    }

    _snapshot.graph.nodes = std::move(nodes);
    _snapshot.graph.links = std::move(links);

    analyzeAudioQuality();
  }

  void PlaybackEngine::analyzeAudioQuality()
  {
    auto& snap = _snapshot;
    snap.quality = AudioQuality::BitwisePerfect;
    snap.qualityTooltip.clear();

    PLAYBACK_LOG_DEBUG("Analyzing audio quality for graph with {} nodes", snap.graph.nodes.size());

    appendLine(snap.qualityTooltip, "Audio Routing Analysis:");

    // 1. Build the strict linear path (The "Total Order" of the pipeline)
    std::vector<AudioNode*> path;
    {
      auto currentId = std::string{"rs-decoder"};
      std::set<std::string> visited;
      while (!currentId.empty() && !visited.contains(currentId))
      {
        visited.insert(currentId);
        auto it = std::ranges::find_if(snap.graph.nodes, [&](auto const& n) { return n.id == currentId; });
        if (it == snap.graph.nodes.end()) break;

        path.push_back(&(*it));

        std::string nextId;
        for (auto const& link : snap.graph.links)
        {
          if (link.isActive && link.sourceId == currentId)
          {
            nextId = link.destId;
            break;
          }
        }
        currentId = nextId;
      }
    }

    // 3. Identify all input sources for each node to detect mixing later
    auto inputSources = std::unordered_map<std::string, std::set<std::string>>{};
    for (auto const& link : snap.graph.links)
    {
      if (link.isActive) inputSources[link.destId].insert(link.sourceId);
    }

    // 4. Single-pass linear analysis through the path
    for (size_t i = 0; i < path.size(); ++i)
    {
      auto* node = path[i];

      // --- A. Node internal state ---
      if (node->isLossySource)
      {
        appendLine(snap.qualityTooltip, std::format("• Source: Lossy format ({})", node->name));
        snap.quality = std::max(snap.quality, AudioQuality::LossySource);
      }

      if (node->volumeNotUnity)
      {
        appendLine(snap.qualityTooltip, std::format("• Volume: Modification at {}", node->name));
        snap.quality = std::max(snap.quality, AudioQuality::LinearIntervention);
      }

      if (node->isMuted)
      {
        appendLine(snap.qualityTooltip, std::format("• Status: {} is MUTED", node->name));
        snap.quality = std::max(snap.quality, AudioQuality::LinearIntervention);
      }

      // --- B. Node external interference (Mixing) ---
      if (inputSources.contains(node->id))
      {
        auto const& sources = inputSources.at(node->id);
        std::vector<std::string> otherAppNames;
        for (auto const& srcId : sources)
        {
          bool isInternal = std::ranges::any_of(path, [&](auto* p) { return p->id == srcId; });
          if (!isInternal)
          {
            auto it = std::ranges::find_if(snap.graph.nodes, [&](auto const& n) { return n.id == srcId; });
            if (it != snap.graph.nodes.end()) otherAppNames.push_back(it->name);
          }
        }

        if (!otherAppNames.empty())
        {
          std::ranges::sort(otherAppNames);
          auto [first, last] = std::ranges::unique(otherAppNames);
          otherAppNames.erase(first, last);
          std::string apps;
          for (size_t j = 0; j < otherAppNames.size(); ++j)
          {
            apps += otherAppNames[j];
            if (j < otherAppNames.size() - 1) apps += ", ";
          }
          appendLine(snap.qualityTooltip, std::format("• Mixed: {} shared with {}", node->name, apps));
          snap.quality = std::max(snap.quality, AudioQuality::LinearIntervention);
        }
      }

      // --- C. Link transition to next node ---
      if (i < path.size() - 1)
      {
        auto* node = path[i];
        auto* nextNode = path[i + 1];
        if (node->format && nextNode->format)
        {
          auto const& f1 = *node->format;
          auto const& f2 = *nextNode->format;

          if (f1.sampleRate != f2.sampleRate)
          {
            appendLine(snap.qualityTooltip, std::format("• Resampling: {}Hz → {}Hz", f1.sampleRate, f2.sampleRate));
            snap.quality = std::max(snap.quality, AudioQuality::LinearIntervention);
          }

          if (f1.channels != f2.channels)
          {
            appendLine(snap.qualityTooltip, std::format("• Channels: {}ch → {}ch", f1.channels, f2.channels));
            snap.quality = std::max(snap.quality, AudioQuality::LinearIntervention);
          }
          else if (f1.bitDepth != f2.bitDepth || f1.isFloat != f2.isFloat)
          {
            if (isLosslessBitDepthChange(f1, f2))
            {
              appendLine(snap.qualityTooltip,
                         f2.isFloat ? "• Bit-Transparent: Float mapping" : "• Bit-Transparent: Integer padding");
              snap.quality =
                std::max(snap.quality, f2.isFloat ? AudioQuality::LosslessFloat : AudioQuality::LosslessPadded);
            }
            else
            {
              appendLine(
                snap.qualityTooltip, std::format("• Precision: Truncated {}b → {}b", f1.bitDepth, f2.bitDepth));
              snap.quality = std::max(snap.quality, AudioQuality::LinearIntervention);
            }
          }
        }
      }
    }

    if (snap.quality == backend::AudioQuality::BitwisePerfect)
    {
      appendLine(snap.qualityTooltip, "• Signal Path: Byte-perfect from decoder to device");
    }

    // 5. Final summary
    switch (snap.quality)
    {
      case backend::AudioQuality::BitwisePerfect:
        appendLine(snap.qualityTooltip, "\nConclusion: Bit-perfect output");
        break;
      case backend::AudioQuality::LosslessPadded:
      case backend::AudioQuality::LosslessFloat:
        appendLine(snap.qualityTooltip, "\nConclusion: Lossless Conversion");
        break;
      case backend::AudioQuality::LinearIntervention:
        appendLine(snap.qualityTooltip, "\nConclusion: Linear intervention (Resampled/Mixed/Vol)");
        break;
      case backend::AudioQuality::LossySource:
        appendLine(snap.qualityTooltip, "\nConclusion: Lossy source format");
        break;
      case backend::AudioQuality::Clipped:
        appendLine(snap.qualityTooltip, "\nConclusion: Signal clipping detected");
        break;
      case backend::AudioQuality::Unknown: break;
    }
  }

  std::size_t PlaybackEngine::onReadPcm(void* userData, std::span<std::byte> output) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    auto source = self->_source.load(std::memory_order_acquire);
    return source ? source->read(output) : 0;
  }

  bool PlaybackEngine::isSourceDrained(void* userData) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    auto source = self->_source.load(std::memory_order_acquire);
    if (!source) return true;
    auto const drained = source->isDrained();
    if (drained) self->_playbackDrainPending = true;
    return drained;
  }

  void PlaybackEngine::onUnderrun(void* userData) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    ++self->_underrunCount;
  }

  void PlaybackEngine::onPositionAdvanced(void* userData, std::uint32_t frames) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    auto lock = std::unique_lock<std::mutex>{self->_stateMutex, std::try_to_lock};
    if (!lock.owns_lock()) return;

    // Use Engine node format for position calculation
    for (auto const& node : self->_snapshot.graph.nodes)
    {
      if (node.type == backend::AudioNodeType::Engine && node.format && node.format->sampleRate > 0)
      {
        auto const ms = (static_cast<std::uint64_t>(frames) * 1000) / node.format->sampleRate;
        self->_snapshot.positionMs += static_cast<std::uint32_t>(ms);
        break;
      }
    }
  }

  void PlaybackEngine::onDrainComplete(void* userData) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    if (!self->_playbackDrainPending.exchange(false, std::memory_order_relaxed)) return;
    self->_source.store({}, std::memory_order_release);
    auto lock = std::lock_guard<std::mutex>{self->_stateMutex};
    self->_currentTrack.reset();
    self->_backendStarted = false;
    self->_state = TransportState::Idle;
    self->_snapshot = {};
    self->_snapshot.backend = self->_backend ? self->_backend->kind() : backend::BackendKind::None;
  }

  void PlaybackEngine::onGraphChanged(void* userData, backend::AudioGraph const& graph) noexcept
  {
    static_cast<PlaybackEngine*>(userData)->handleGraphChanged(graph);
  }

  void PlaybackEngine::onSourceError(void* userData) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    auto source = self->_source.load(std::memory_order_acquire);
    auto const errorText = source ? source->lastError() : std::string{};
    {
      auto lock = std::lock_guard<std::mutex>{self->_stateMutex};
      if (self->_state == TransportState::Idle) return;
      self->_backendStarted = false;
      self->_playbackDrainPending = false;
      self->_state = TransportState::Error;
      self->_snapshot.state = TransportState::Error;
      self->_snapshot.statusText = errorText.empty() ? "PCM source failed" : errorText;
    }
    if (self->_backend) self->_backend->stop();
  }

} // namespace app::core::playback

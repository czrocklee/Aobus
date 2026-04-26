// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/PlaybackEngine.h"
#include "core/Log.h"

#include "core/playback/AudioDecoderSession.h"
#include "core/playback/MemoryPcmSource.h"
#include "core/playback/StreamingPcmSource.h"

#include <algorithm>
#include <limits>
#include <set>
#include <sstream>

namespace
{
  constexpr std::uint32_t kPrerollTargetMs = 200;
  constexpr std::uint32_t kDecodeHighWatermarkMs = 750;
  constexpr std::uint64_t kMemoryPcmSourceBudgetBytes = 64ULL * 1024ULL * 1024ULL;

  bool isLosslessBitDepthChange(app::core::playback::StreamFormat const& src,
                                app::core::playback::StreamFormat const& dst) noexcept
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

  std::uint64_t bytesPerSecond(app::core::playback::StreamFormat const& format) noexcept
  {
    if (format.sampleRate == 0 || format.channels == 0 || format.bitDepth == 0)
    {
      return 0;
    }

    auto const bytesPerSample = (format.bitDepth == 24U) ? 3U : (format.bitDepth > 16U) ? 4U : 2U;
    return static_cast<std::uint64_t>(format.sampleRate) * format.channels * bytesPerSample;
  }

  std::uint64_t estimatedDecodedBytes(app::core::playback::DecodedStreamInfo const& info) noexcept
  {
    auto const rate = bytesPerSecond(info.outputFormat);
    if (rate == 0 || info.durationMs == 0) return 0;
    return (static_cast<std::uint64_t>(info.durationMs) * rate) / 1000U;
  }

  bool shouldUseMemoryPcmSource(app::core::playback::DecodedStreamInfo const& info) noexcept
  {
    auto const decodedBytes = estimatedDecodedBytes(info);
    return decodedBytes > 0 && decodedBytes <= kMemoryPcmSourceBudgetBytes;
  }
} // namespace

namespace app::core::playback
{

  PlaybackEngine::PlaybackEngine(std::unique_ptr<IAudioBackend> backend)
    : _backend{std::move(backend)}
  {
    _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
  }

  PlaybackEngine::~PlaybackEngine()
  {
    stop();
  }

  void PlaybackEngine::setBackend(std::unique_ptr<IAudioBackend> backend, std::string deviceId)
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

    auto callbacks = AudioRenderCallbacks{};
    callbacks.userData = this;
    callbacks.readPcm = &PlaybackEngine::onReadPcm;
    callbacks.isSourceDrained = &PlaybackEngine::isSourceDrained;
    callbacks.onUnderrun = &PlaybackEngine::onUnderrun;
    callbacks.onPositionAdvanced = &PlaybackEngine::onPositionAdvanced;
    callbacks.onDrainComplete = &PlaybackEngine::onDrainComplete;
    callbacks.onGraphChanged = &PlaybackEngine::onGraphChanged;
    callbacks.onBackendError = &PlaybackEngine::onBackendError;

    auto source = std::shared_ptr<IPcmSource>{};
    auto backendFormat = StreamFormat{};

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
                                 std::shared_ptr<IPcmSource>& source,
                                 StreamFormat& backendFormat)
  {
    auto outputFormat = StreamFormat{};
    outputFormat.sampleRate = 0; // Use native
    outputFormat.channels = 0;   // Use native
    outputFormat.bitDepth = 0;   // Use native
    outputFormat.isFloat = false;
    outputFormat.isInterleaved = true;

    auto decoder = createAudioDecoderSession(outputFormat);
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
      auto memorySource = std::make_shared<MemoryPcmSource>(std::move(decoder), info);
      if (!memorySource->initialize())
      {
        _snapshot.statusText = memorySource->lastError();
        return false;
      }
      source = std::move(memorySource);
    }
    else
    {
      PcmSourceCallbacks sourceCallbacks;
      sourceCallbacks.userData = this;
      sourceCallbacks.onError = &PlaybackEngine::onSourceError;
      auto streamingSource = std::make_shared<StreamingPcmSource>(
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

  void PlaybackEngine::handleGraphChanged(AudioGraph const& backendGraph)
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
    snap.quality = AudioQuality::BitPerfect;
    snap.qualityTooltip.clear();

    PLAYBACK_LOG_DEBUG("Analyzing audio quality for graph with {} nodes", snap.graph.nodes.size());

    appendLine(snap.qualityTooltip, "Audio Routing Analysis");

    // 1. Identify all nodes in the RockStudio path
    std::set<std::string> rsPathNodes;
    {
      std::string current = "rs-decoder";
      std::set<std::string> seen;
      while (!current.empty() && !seen.contains(current))
      {
        seen.insert(current);
        rsPathNodes.insert(current);

        std::string next;
        for (auto const& link : snap.graph.links)
        {
          if (link.isActive && link.sourceId == current)
          {
            next = link.destId;
            break;
          }
        }
        current = next;
      }
    }

    // 2. Check for Mixing (Multiple DISTINCT sources feeding a node)
    auto inputSources = std::unordered_map<std::string, std::set<std::string>>{};
    for (auto const& link : snap.graph.links)
    {
      if (link.isActive) inputSources[link.destId].insert(link.sourceId);
    }

    bool hasMixing = false;
    for (auto const& [nodeId, sources] : inputSources)
    {
      if (sources.size() > 1)
      {
        auto it =
          std::find_if(snap.graph.nodes.begin(), snap.graph.nodes.end(), [&](auto const& n) { return n.id == nodeId; });
        if (it != snap.graph.nodes.end())
        {
          bool rsPathIncluded = false;
          std::vector<std::string> otherAppNames;

          for (auto const& sourceId : sources)
          {
            if (rsPathNodes.contains(sourceId))
            {
              rsPathIncluded = true;
            }
            else
            {
              auto sourceNode = std::find_if(
                snap.graph.nodes.begin(), snap.graph.nodes.end(), [&](auto const& n) { return n.id == sourceId; });
              if (sourceNode != snap.graph.nodes.end())
              {
                otherAppNames.push_back(sourceNode->name);
              }
            }
          }

          if (rsPathIncluded && !otherAppNames.empty())
          {
            std::string apps;
            std::sort(otherAppNames.begin(), otherAppNames.end());
            otherAppNames.erase(std::unique(otherAppNames.begin(), otherAppNames.end()), otherAppNames.end());

            for (std::size_t i = 0; i < otherAppNames.size(); ++i)
            {
              apps += otherAppNames[i];
              if (i < otherAppNames.size() - 1) apps += ", ";
            }
            appendLine(snap.qualityTooltip, "• Mixed: Sharing " + it->name + " with " + apps);
            hasMixing = true;
          }
        }
      }
    }
    if (hasMixing) snap.quality = std::max(snap.quality, AudioQuality::Mixed);

    // 2. Traversal and Format Analysis
    std::string currentNodeId = "rs-decoder";
    std::set<std::string> visited;

    PLAYBACK_LOG_DEBUG("Starting graph traversal from 'rs-decoder'");

    while (!currentNodeId.empty())
    {
      if (visited.contains(currentNodeId))
      {
        PLAYBACK_LOG_WARN("Graph cycle detected at '{}'", currentNodeId);
        break;
      }
      visited.insert(currentNodeId);

      auto const nodeIt = std::find_if(
        snap.graph.nodes.begin(), snap.graph.nodes.end(), [&](auto const& n) { return n.id == currentNodeId; });
      if (nodeIt == snap.graph.nodes.end())
      {
        PLAYBACK_LOG_DEBUG("Traversal reached end at missing node '{}'", currentNodeId);
        break;
      }

      auto const& node = *nodeIt;
      PLAYBACK_LOG_DEBUG("  Node: {} [type={}]", node.name, static_cast<int>(node.type));

      if (node.volumeNotUnity)
      {
        PLAYBACK_LOG_DEBUG("    - Volume modification detected");
        appendLine(snap.qualityTooltip, "• Volume transformation at " + node.name);
        snap.quality = std::max(snap.quality, AudioQuality::Lossless);
      }
      if (node.isMuted)
      {
        PLAYBACK_LOG_DEBUG("    - Node is muted");
        appendLine(snap.qualityTooltip, "• Node is muted: " + node.name);
        snap.quality = std::max(snap.quality, AudioQuality::Mixed);
      }

      // Find next node
      std::string nextNodeId;
      for (auto const& link : snap.graph.links)
      {
        if (link.isActive && link.sourceId == currentNodeId)
        {
          auto const nextNodeIt = std::find_if(
            snap.graph.nodes.begin(), snap.graph.nodes.end(), [&](auto const& n) { return n.id == link.destId; });
          if (nextNodeIt != snap.graph.nodes.end())
          {
            auto const& nextNode = *nextNodeIt;

            // Compare formats if both are present
            if (node.format && nextNode.format)
            {
              auto const& f1 = *node.format;
              auto const& f2 = *nextNode.format;

              PLAYBACK_LOG_DEBUG("    - Link format comparison: [{}Hz/{}b/{}ch] -> [{}Hz/{}b/{}ch]",
                                 f1.sampleRate,
                                 f1.bitDepth,
                                 f1.channels,
                                 f2.sampleRate,
                                 f2.bitDepth,
                                 f2.channels);

              if (f1.sampleRate != f2.sampleRate)
              {
                PLAYBACK_LOG_DEBUG("      - Resampling detected");
                appendLine(snap.qualityTooltip,
                           "• Resampling: " + std::to_string(f1.sampleRate) + " → " + std::to_string(f2.sampleRate));
                snap.quality = std::max(snap.quality, AudioQuality::Resampled);
              }
              if (f1.channels != f2.channels)
              {
                PLAYBACK_LOG_DEBUG("      - Channel mixing detected");
                appendLine(snap.qualityTooltip,
                           "• Mixing channels: " + std::to_string(f1.channels) + " → " + std::to_string(f2.channels));
                snap.quality = std::max(snap.quality, AudioQuality::Mixed);
              }
              else if (f1.bitDepth != f2.bitDepth || f1.isFloat != f2.isFloat)
              {
                if (isLosslessBitDepthChange(f1, f2))
                {
                  PLAYBACK_LOG_DEBUG("      - Lossless bit-depth upscale");
                  appendLine(snap.qualityTooltip, "• Bit-depth upscaling (lossless)");
                  snap.quality = std::max(snap.quality, AudioQuality::Lossless);
                }
                else
                {
                  PLAYBACK_LOG_DEBUG("      - Precision loss (bit-depth truncate)");
                  appendLine(snap.qualityTooltip,
                             "• Precision loss: " + std::to_string(f1.bitDepth) + " → " + std::to_string(f2.bitDepth));
                  snap.quality = std::max(snap.quality, AudioQuality::Lossy);
                }
              }
            }

            nextNodeId = link.destId;
            break;
          }
        }
      }
      currentNodeId = nextNodeId;
    }

    PLAYBACK_LOG_DEBUG("Analysis complete. Final quality: {}", static_cast<int>(snap.quality));

    // Final Summary
    switch (snap.quality)
    {
      case AudioQuality::BitPerfect: appendLine(snap.qualityTooltip, "\nStatus: Bit-Perfect"); break;
      case AudioQuality::Lossless: appendLine(snap.qualityTooltip, "\nStatus: Lossless Conversion"); break;
      case AudioQuality::Resampled: appendLine(snap.qualityTooltip, "\nStatus: High Quality (Resampled)"); break;
      case AudioQuality::Mixed: appendLine(snap.qualityTooltip, "\nStatus: Mixed / Shared"); break;
      case AudioQuality::Lossy: appendLine(snap.qualityTooltip, "\nStatus: Quality Degraded"); break;
      case AudioQuality::Unknown: break;
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
      if (node.type == AudioNodeType::Engine && node.format && node.format->sampleRate > 0)
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
    self->_snapshot.backend = self->_backend ? self->_backend->kind() : BackendKind::None;
  }

  void PlaybackEngine::onGraphChanged(void* userData, AudioGraph const& graph) noexcept
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

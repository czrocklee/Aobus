// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/PlaybackController.h"
#include "core/Log.h"
#include "core/backend/IBackendManager.h"
#include "core/backend/NullBackend.h"
#include "core/playback/PlaybackEngine.h"

#include <algorithm>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <unordered_map>

namespace app::core::playback
{
  namespace
  {
    void appendLine(std::string& text, std::string_view line)
    {
      if (line.empty())
      {
        return;
      }
      if (!text.empty())
      {
        text += '\n';
      }
      text += line;
    }

    bool isLosslessBitDepthChange(app::core::AudioFormat const& src, app::core::AudioFormat const& dst) noexcept
    {
      if (src.isFloat == dst.isFloat)
      {
        auto const srcBits = src.validBits != 0 ? src.validBits : src.bitDepth;
        auto const dstBits = dst.validBits != 0 ? dst.validBits : dst.bitDepth;
        return srcBits <= dstBits;
      }
      if (!src.isFloat && dst.isFloat)
      {
        auto const srcBits = src.validBits != 0 ? src.validBits : src.bitDepth;
        if (dst.bitDepth == 32)
        {
          return srcBits <= 24;
        }
        if (dst.bitDepth == 64)
        {
          return srcBits <= 32;
        }
      }
      return false;
    }
  }
  PlaybackController::PlaybackController(std::shared_ptr<IMainThreadDispatcher> dispatcher)
    : _dispatcher(std::move(dispatcher))
  {
    // Start with a NullBackend until a manager provides something real
    _engine = std::make_unique<PlaybackEngine>(std::make_unique<backend::NullBackend>(),
                                               backend::AudioDevice{.id = "null",
                                                                    .displayName = "None",
                                                                    .description = "No audio output selected",
                                                                    .backendKind = backend::BackendKind::None,
                                                                    .capabilities = {}},
                                               _dispatcher);

    _engine->setOnTrackEnded(
      [this]()
      {
        if (_onTrackEnded)
        {
          _onTrackEnded();
        }
      });

    _engine->setOnRouteChanged(
      [this](EngineRouteSnapshot const& snapshot)
      {
        // Capture generation to prevent stale updates
        auto const generation = _playbackGeneration;
        if (_dispatcher)
        {
          _dispatcher->dispatch([this, snapshot, generation]() { handleRouteChanged(snapshot, generation); });
        }
        else
        {
          handleRouteChanged(snapshot, generation);
        }
      });
  }

  void PlaybackController::setTrackEndedCallback(std::function<void()> callback)
  {
    _onTrackEnded = std::move(callback);
  }

  PlaybackController::~PlaybackController()
  {
    if (_engine)
    {
      _engine->setOnRouteChanged(nullptr);
    }
  }

  void PlaybackController::addManager(std::unique_ptr<backend::IBackendManager> manager)
  {
    if (!manager)
    {
      return;
    }

    manager->setDevicesChangedCallback([this] { _backendsDirty = true; });
    _managers.push_back(std::move(manager));
    _backendsDirty = true;
  }

  void PlaybackController::play(TrackPlaybackDescriptor const& descriptor)
  {
    _playbackGeneration++;
    _cachedEngineRoute = {};
    _cachedSystemGraph = {};
    _mergedGraph = {};
    _quality = backend::AudioQuality::Unknown;
    _qualityTooltip.clear();
    _graphSubscription.reset();
    _engine->play(descriptor);
  }

  void PlaybackController::setOutput(backend::BackendKind kind, std::string_view deviceId)
  {
    auto const currentSnap = _engine->snapshot();

    // 1. Check if we already have this output active
    if (kind == currentSnap.backend && deviceId == currentSnap.currentDeviceId)
    {
      return;
    }

    if (_allDevices.empty())
    {
      snapshot();
    }

    // 2. Find the AudioDevice matching the kind and id from our cache
    auto const it = std::ranges::find_if(
      _allDevices, [&](backend::AudioDevice const& dev) { return dev.backendKind == kind && dev.id == deviceId; });

    if (it == _allDevices.end())
    {
      PLAYBACK_LOG_ERROR("PlaybackController: Requested unknown output {}:{}", backendKindToId(kind), deviceId);
      return;
    }

    auto const& targetDevice = *it;

    // 3. Find the manager object that can handle this BackendKind
    for (auto const& manager : _managers)
    {
      auto devices = manager->enumerateDevices();
      auto const found = std::ranges::contains(devices, targetDevice);

      if (found)
      {
        if (auto backend = manager->createBackend(targetDevice))
        {
          _activeManager = manager.get();
          _playbackGeneration++;
          _engine->setBackend(std::move(backend), targetDevice);
          return;
        }
      }
    }

    PLAYBACK_LOG_ERROR(
      "PlaybackController: Failed to create backend for output {}:{}", backendKindToId(kind), deviceId);
  }

  void PlaybackController::pause()
  {
    _engine->pause();
  }

  void PlaybackController::resume()
  {
    _engine->resume();
  }

  void PlaybackController::stop()
  {
    _playbackGeneration++;
    _cachedEngineRoute = {};
    _cachedSystemGraph = {};
    _mergedGraph = {};
    _quality = backend::AudioQuality::Unknown;
    _qualityTooltip.clear();
    _graphSubscription.reset();
    _engine->stop();
  }

  void PlaybackController::seek(std::uint32_t positionMs)
  {
    _engine->seek(positionMs);
  }

  PlaybackSnapshot PlaybackController::snapshot() const
  {
    auto snap = _engine->snapshot();

    if (_backendsDirty.exchange(false))
    {
      auto allDevices = std::vector<backend::AudioDevice>{};
      for (auto const& manager : _managers)
      {
        auto devices = manager->enumerateDevices();
        allDevices.insert(
          allDevices.end(), std::make_move_iterator(devices.begin()), std::make_move_iterator(devices.end()));
      }

      // Group devices by BackendKind
      std::map<backend::BackendKind, std::vector<backend::AudioDevice>> groups;
      for (auto const& dev : allDevices)
      {
        groups[dev.backendKind].push_back(dev);
      }

      auto snapshots = std::vector<BackendSnapshot>{};
      for (auto& [kind, devices] : groups)
      {
        snapshots.push_back({.kind = kind,
                             .displayName = std::string(backendDisplayName(kind)),
                             .shortName = std::string(backendShortName(kind)),
                             .id = std::string(backendKindToId(kind)),
                             .devices = std::move(devices)});
      }

      _cachedBackends = std::move(snapshots);
      _allDevices = allDevices; // Store flat list for setOutput lookup
    }

    snap.availableBackends = _cachedBackends;

    // Inject controller-owned fields into the snapshot returned to the UI
    snap.graph = _mergedGraph;
    snap.quality = _quality;
    snap.qualityTooltip = _qualityTooltip;

    return snap;
  }

  void PlaybackController::handleRouteChanged(EngineRouteSnapshot const& snapshot, std::uint64_t generation)
  {
    if (generation != _playbackGeneration)
    {
      return;
    }
    _cachedEngineRoute = snapshot;

    // Check if we have a valid anchor and manager to subscribe to the system graph
    if (_cachedEngineRoute.anchor.has_value() && _activeManager)
    {
      if (!_graphSubscription)
      {
        _graphSubscription = _activeManager->subscribeGraph(
          _cachedEngineRoute.anchor->id,
          [this, generation](backend::AudioGraph const& graph)
          {
            if (_dispatcher)
            {
              _dispatcher->dispatch([this, graph, generation]() { handleSystemGraphChanged(graph, generation); });
            }
            else
            {
              handleSystemGraphChanged(graph, generation);
            }
          });
      }
    }
    else
    {
      _graphSubscription.reset();
      _cachedSystemGraph = {};
    }

    updateMergedGraph();
  }

  void PlaybackController::handleSystemGraphChanged(backend::AudioGraph const& graph, std::uint64_t generation)
  {
    if (generation != _playbackGeneration)
    {
      return;
    }
    _cachedSystemGraph = graph;
    updateMergedGraph();
  }

  void PlaybackController::updateMergedGraph()
  {
    _mergedGraph = _cachedEngineRoute.graph;

    // Find the engine output format to enrich system nodes that might be missing it (like ALSA)
    std::optional<AudioFormat> engineFormat;
    for (auto const& node : _cachedEngineRoute.graph.nodes)
    {
      if (node.id == "rs-engine")
      {
        engineFormat = node.format;
        break;
      }
    }

    // Add system graph nodes and links
    for (auto node : _cachedSystemGraph.nodes)
    {
      if (!node.format.has_value() && engineFormat.has_value())
      {
        node.format = engineFormat;
      }
      _mergedGraph.nodes.push_back(node);
    }
    for (auto const& link : _cachedSystemGraph.links)
    {
      _mergedGraph.links.push_back(link);
    }

    // Find the backend stream node to bridge the engine to
    std::string streamNodeId;
    for (auto const& node : _cachedSystemGraph.nodes)
    {
      if (node.type == backend::AudioNodeType::Stream)
      {
        streamNodeId = node.id;
        break;
      }
    }

    if (!streamNodeId.empty())
    {
      _mergedGraph.links.push_back({.sourceId = "rs-engine", .destId = streamNodeId, .isActive = true});
    }

    analyzeAudioQuality();
  }

  void PlaybackController::analyzeAudioQuality()
  {
    // Now analyze the merged graph
    _quality = backend::AudioQuality::BitwisePerfect;
    _qualityTooltip.clear();

    if (_mergedGraph.nodes.empty())
    {
      _quality = backend::AudioQuality::Unknown;
      return;
    }

    appendLine(_qualityTooltip, "Audio Routing Analysis:");

    // 1. Build linear path
    std::vector<backend::AudioNode*> path;
    {
      auto currentId = std::string{"rs-decoder"};
      std::set<std::string> visited;
      while (!currentId.empty() && !visited.contains(currentId))
      {
        visited.insert(currentId);
        if (auto it = std::ranges::find(_mergedGraph.nodes, currentId, &backend::AudioNode::id);
            it == _mergedGraph.nodes.end())
        {
          break;
        }
        else
        {
          path.push_back(&(*it));

          std::string nextId;
          for (auto const& link : _mergedGraph.links)
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
    }

    // 2. Identify mixing sources
    auto inputSources = std::unordered_map<std::string, std::set<std::string>>{};
    for (auto const& link : _mergedGraph.links)
    {
      if (link.isActive)
      {
        inputSources[link.destId].insert(link.sourceId);
      }
    }

    for (size_t i = 0; i < path.size(); ++i)
    {
      auto* node = path[i];

      if (node->isLossySource)
      {
        appendLine(_qualityTooltip, std::format("• Source: Lossy format ({})", node->name));
        _quality = std::max(_quality, backend::AudioQuality::LossySource);
      }

      if (node->volumeNotUnity)
      {
        appendLine(_qualityTooltip, std::format("• Volume: Modification at {}", node->name));
        _quality = std::max(_quality, backend::AudioQuality::LinearIntervention);
      }

      if (node->isMuted)
      {
        appendLine(_qualityTooltip, std::format("• Status: {} is MUTED", node->name));
        _quality = std::max(_quality, backend::AudioQuality::LinearIntervention);
      }

      if (inputSources.contains(node->id))
      {
        auto const& sources = inputSources.at(node->id);
        std::vector<std::string> otherAppNames;
        for (auto const& srcId : sources)
        {
          if (bool isInternal = std::ranges::contains(path, srcId, &backend::AudioNode::id); !isInternal)
          {
            if (auto it = std::ranges::find(_mergedGraph.nodes, srcId, &backend::AudioNode::id);
                it != _mergedGraph.nodes.end())
            {
              otherAppNames.push_back(it->name);
            }
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
            if (j < otherAppNames.size() - 1)
            {
              apps += ", ";
            }
          }
          appendLine(_qualityTooltip, std::format("• Mixed: {} shared with {}", node->name, apps));
          _quality = std::max(_quality, backend::AudioQuality::LinearIntervention);
        }
      }

      if (i < path.size() - 1)
      {
        auto* nextNode = path[i + 1];
        if (node->format && nextNode->format)
        {
          auto const& f1 = *node->format;
          auto const& f2 = *nextNode->format;

          if (f1.sampleRate != f2.sampleRate)
          {
            appendLine(_qualityTooltip, std::format("• Resampling: {}Hz → {}Hz", f1.sampleRate, f2.sampleRate));
            _quality = std::max(_quality, backend::AudioQuality::LinearIntervention);
          }
          if (f1.channels != f2.channels)
          {
            appendLine(_qualityTooltip, std::format("• Channels: {}ch → {}ch", f1.channels, f2.channels));
            _quality = std::max(_quality, backend::AudioQuality::LinearIntervention);
          }
          else if (f1.bitDepth != f2.bitDepth || f1.isFloat != f2.isFloat)
          {
            if (isLosslessBitDepthChange(f1, f2))
            {
              appendLine(_qualityTooltip,
                         f2.isFloat ? "• Bit-Transparent: Float mapping" : "• Bit-Transparent: Integer padding");
              _quality = std::max(
                _quality, f2.isFloat ? backend::AudioQuality::LosslessFloat : backend::AudioQuality::LosslessPadded);
            }
            else
            {
              appendLine(_qualityTooltip, std::format("• Precision: Truncated {}b → {}b", f1.bitDepth, f2.bitDepth));
              _quality = std::max(_quality, backend::AudioQuality::LinearIntervention);
            }
          }
        }
      }
    }

    if (_quality == backend::AudioQuality::BitwisePerfect)
    {
      appendLine(_qualityTooltip, "• Signal Path: Byte-perfect from decoder to device");
    }

    switch (_quality)
    {
      case backend::AudioQuality::BitwisePerfect:
        appendLine(_qualityTooltip, "\nConclusion: Bit-perfect output");
        break;
      case backend::AudioQuality::LosslessPadded:
      case backend::AudioQuality::LosslessFloat:
        appendLine(_qualityTooltip, "\nConclusion: Lossless Conversion");
        break;
      case backend::AudioQuality::LinearIntervention:
        appendLine(_qualityTooltip, "\nConclusion: Linear intervention (Resampled/Mixed/Vol)");
        break;
      case backend::AudioQuality::LossySource: appendLine(_qualityTooltip, "\nConclusion: Lossy source format"); break;
      case backend::AudioQuality::Clipped: appendLine(_qualityTooltip, "\nConclusion: Signal clipping detected"); break;
      case backend::AudioQuality::Unknown: break;
    }
  }

} // namespace app::core::playback

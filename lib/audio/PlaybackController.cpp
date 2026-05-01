// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/audio/IBackendManager.h>
#include <rs/audio/NullBackend.h>
#include <rs/audio/PlaybackController.h>
#include <rs/audio/PlaybackEngine.h>
#include <rs/utility/Log.h>

#include <algorithm>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <unordered_map>

namespace rs::audio
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

    bool isLosslessBitDepthChange(rs::audio::AudioFormat const& src, rs::audio::AudioFormat const& dst) noexcept
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

  PlaybackController::PlaybackController(std::shared_ptr<rs::IMainThreadDispatcher> dispatcher)
    : _dispatcher(std::move(dispatcher))
  {
    // Start with a NullBackend until a manager provides something real
    _engine = std::make_unique<PlaybackEngine>(std::make_unique<rs::audio::NullBackend>(),
                                               rs::audio::AudioDevice{.id = "null",
                                                                      .displayName = "None",
                                                                      .description = "No audio output selected",
                                                                      .backendKind = rs::audio::BackendKind::None,
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

  void PlaybackController::addManager(std::unique_ptr<rs::audio::IBackendManager> manager)
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
    _quality = rs::audio::AudioQuality::Unknown;
    _qualityTooltip.clear();
    _graphSubscription.reset();
    _engine->play(descriptor);
  }

  void PlaybackController::setOutput(rs::audio::BackendKind kind, std::string_view deviceId)
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
      _allDevices, [&](rs::audio::AudioDevice const& dev) { return dev.backendKind == kind && dev.id == deviceId; });

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
    _quality = rs::audio::AudioQuality::Unknown;
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
      auto allDevices = std::vector<rs::audio::AudioDevice>{};

      for (auto const& manager : _managers)
      {
        auto devices = manager->enumerateDevices();
        allDevices.insert(
          allDevices.end(), std::make_move_iterator(devices.begin()), std::make_move_iterator(devices.end()));
      }

      // Group devices by BackendKind
      std::map<rs::audio::BackendKind, std::vector<rs::audio::AudioDevice>> groups;

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
    if (_cachedEngineRoute.anchor.has_value() && _activeManager != nullptr)
    {
      if (!_graphSubscription)
      {
        _graphSubscription = _activeManager->subscribeGraph(
          _cachedEngineRoute.anchor->id,
          [this, generation](rs::audio::AudioGraph const& graph)
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

  void PlaybackController::handleSystemGraphChanged(rs::audio::AudioGraph const& graph, std::uint64_t generation)
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
      if (node.type == rs::audio::AudioNodeType::Stream)
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

  std::vector<rs::audio::AudioNode const*> PlaybackController::findPlaybackPath(std::string const& startId) const
  {
    std::vector<rs::audio::AudioNode const*> path;
    auto currentId = startId;
    std::set<std::string> visited;

    while (!currentId.empty() && !visited.contains(currentId))
    {
      visited.insert(currentId);

      auto const it = std::ranges::find(_mergedGraph.nodes, currentId, &rs::audio::AudioNode::id);

      if (it == _mergedGraph.nodes.end())
      {
        break;
      }

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

    return path;
  }

  void PlaybackController::processInputSources(
    rs::audio::AudioNode const& node,
    std::span<rs::audio::AudioNode const* const> path,
    std::unordered_map<std::string, std::set<std::string>> const& inputSources)
  {
    if (inputSources.contains(node.id))
    {
      auto const& sources = inputSources.at(node.id);
      std::vector<std::string> otherAppNames;

      for (auto const& srcId : sources)
      {
        if (bool const isInternal = std::ranges::contains(path, srcId, &rs::audio::AudioNode::id); !isInternal)
        {
          if (auto const it = std::ranges::find(_mergedGraph.nodes, srcId, &rs::audio::AudioNode::id);
              it != _mergedGraph.nodes.end())
          {
            otherAppNames.push_back(it->name);
          }
        }
      }

      if (!otherAppNames.empty())
      {
        std::ranges::sort(otherAppNames);
        auto const [first, last] = std::ranges::unique(otherAppNames);
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

        appendLine(_qualityTooltip, std::format("• Mixed: {} shared with {}", node.name, apps));
        _quality = std::max(_quality, rs::audio::AudioQuality::LinearIntervention);
      }
    }
  }

  void PlaybackController::assessNodeQuality(rs::audio::AudioNode const& node, rs::audio::AudioNode const* nextNode)
  {
    if (node.isLossySource)
    {
      appendLine(_qualityTooltip, std::format("• Source: Lossy format ({})", node.name));
      _quality = std::max(_quality, rs::audio::AudioQuality::LossySource);
    }

    if (node.volumeNotUnity)
    {
      appendLine(_qualityTooltip, std::format("• Volume: Modification at {}", node.name));
      _quality = std::max(_quality, rs::audio::AudioQuality::LinearIntervention);
    }

    if (node.isMuted)
    {
      appendLine(_qualityTooltip, std::format("• Status: {} is MUTED", node.name));
      _quality = std::max(_quality, rs::audio::AudioQuality::LinearIntervention);
    }

    if (nextNode != nullptr)
    {
      if (node.format && nextNode->format)
      {
        auto const& f1 = *node.format;
        auto const& f2 = *nextNode->format;

        if (f1.sampleRate != f2.sampleRate)
        {
          appendLine(_qualityTooltip, std::format("• Resampling: {}Hz → {}Hz", f1.sampleRate, f2.sampleRate));
          _quality = std::max(_quality, rs::audio::AudioQuality::LinearIntervention);
        }

        if (f1.channels != f2.channels)
        {
          appendLine(_qualityTooltip, std::format("• Channels: {}ch → {}ch", f1.channels, f2.channels));
          _quality = std::max(_quality, rs::audio::AudioQuality::LinearIntervention);
        }
        else if (f1.bitDepth != f2.bitDepth || f1.isFloat != f2.isFloat)
        {
          if (isLosslessBitDepthChange(f1, f2))
          {
            appendLine(
              _qualityTooltip, f2.isFloat ? "• Bit-Transparent: Float mapping" : "• Bit-Transparent: Integer padding");
            _quality = std::max(
              _quality, f2.isFloat ? rs::audio::AudioQuality::LosslessFloat : rs::audio::AudioQuality::LosslessPadded);
          }
          else
          {
            appendLine(_qualityTooltip, std::format("• Precision: Truncated {}b → {}b", f1.bitDepth, f2.bitDepth));
            _quality = std::max(_quality, rs::audio::AudioQuality::LinearIntervention);
          }
        }
      }
    }
  }

  void PlaybackController::analyzeAudioQuality()
  {
    // Now analyze the merged graph
    _quality = rs::audio::AudioQuality::BitwisePerfect;
    _qualityTooltip.clear();

    if (_mergedGraph.nodes.empty())
    {
      _quality = rs::audio::AudioQuality::Unknown;
      return;
    }

    appendLine(_qualityTooltip, "Audio Routing Analysis:");

    // 1. Build linear path
    auto const path = findPlaybackPath("rs-decoder");

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
      auto const* const node = path[i];
      auto const* const nextNode = (i < path.size() - 1) ? path[i + 1] : nullptr;

      assessNodeQuality(*node, nextNode);
      processInputSources(*node, path, inputSources);
    }

    if (_quality == rs::audio::AudioQuality::BitwisePerfect)
    {
      appendLine(_qualityTooltip, "• Signal Path: Byte-perfect from decoder to device");
    }

    switch (_quality)
    {
      case rs::audio::AudioQuality::BitwisePerfect:
        appendLine(_qualityTooltip, "\nConclusion: Bit-perfect output");
        break;

      case rs::audio::AudioQuality::LosslessPadded:
      case rs::audio::AudioQuality::LosslessFloat:
        appendLine(_qualityTooltip, "\nConclusion: Lossless Conversion");
        break;

      case rs::audio::AudioQuality::LinearIntervention:
        appendLine(_qualityTooltip, "\nConclusion: Linear intervention (Resampled/Mixed/Vol)");
        break;

      case rs::audio::AudioQuality::LossySource:
        appendLine(_qualityTooltip, "\nConclusion: Lossy source format");
        break;

      case rs::audio::AudioQuality::Clipped:
        appendLine(_qualityTooltip, "\nConclusion: Signal clipping detected");
        break;

      case rs::audio::AudioQuality::Unknown: break;
    }
  }

} // namespace rs::audio

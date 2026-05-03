// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <ao/audio/Engine.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Player.h>
#include <ao/utility/Log.h>

#include <algorithm>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <unordered_map>

namespace ao::audio
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

    bool isLosslessBitDepthChange(Format const& src, Format const& dst) noexcept
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

  Player::Player(std::shared_ptr<ao::IMainThreadDispatcher> dispatcher)
    : _dispatcher{std::move(dispatcher)}
  {
    // Start with a NullBackend until a provider provides something real
    _engine = std::make_unique<Engine>(std::make_unique<NullBackend>(),
                                       Device{.id = DeviceId{"null"},
                                              .displayName = "None",
                                              .description = "No audio output selected",
                                              .backendId = kBackendNone,
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
      [this](Engine::RouteStatus const& snapshot)
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

  void Player::setTrackEndedCallback(std::function<void()> callback)
  {
    _onTrackEnded = std::move(callback);
  }

  Player::~Player()
  {
    if (_engine)
    {
      _engine->setOnRouteChanged(nullptr);
    }
  }

  void Player::addProvider(std::unique_ptr<IBackendProvider> provider)
  {
    if (!provider)
    {
      return;
    }

    auto record = std::make_unique<ProviderRecord>();
    record->provider = std::move(provider);

    auto* const providerPtr = record->provider.get();
    auto* const recordPtr = record.get();

    _providers.push_back(std::move(record));

    recordPtr->subscription = providerPtr->subscribeDevices(
      [this, providerPtr, recordPtr](std::vector<Device> const& devices)
      {
        if (_dispatcher)
        {
          _dispatcher->dispatch(
            [this, providerPtr, recordPtr, devices]()
            {
              recordPtr->devices = devices;
              handleDevicesChanged(providerPtr, devices);
            });
        }
        else
        {
          recordPtr->devices = devices;
          handleDevicesChanged(providerPtr, devices);
        }

        return true; // Wait, this is a lambda return, but still a block.
      });
  }

  void Player::play(TrackPlaybackDescriptor const& descriptor)
  {
    if (!isReady())
    {
      AUDIO_LOG_WARN("Player: Playback ignored because audio backend is not ready (pending discovery)");
      return;
    }

    _playbackGeneration++;
    _cachedRouteStatus = {};
    _cachedSystemGraph = {};
    _mergedGraph = {};
    _quality = Quality::Unknown;
    _qualityTooltip.clear();
    _graphSubscription.reset();
    _currentTrack = descriptor;
    _engine->play(descriptor);
  }

  void Player::setOutput(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile)
  {
    auto const currentSnap = _engine->status();

    // 1. Check if we already have this output active
    if (backend == currentSnap.backendId && profile == currentSnap.profileId && deviceId == currentSnap.currentDeviceId)
    {
      _pendingOutput.reset();
      return;
    }

    // 2. Find the Device matching the kind and id from our cache
    auto const it = std::ranges::find_if(
      _allDevices, [&](Device const& dev) { return dev.backendId == backend && dev.id == deviceId; });

    if (it == _allDevices.end())
    {
      // If we don't have it yet, store it as pending.
      _pendingOutput = PendingOutput{.backend = backend, .deviceId = deviceId, .profile = profile};
      AUDIO_LOG_DEBUG("Player: Requested output {}:{} not yet available, pending discovery", backend, deviceId);
      return;
    }

    // Found it! Clear any pending output.
    _pendingOutput.reset();

    // 3. Find the provider object that can handle this BackendId
    auto const recordIt = std::ranges::find_if(
      _providers, [&](auto const& record) { return record->provider->status().metadata.id == backend; });

    if (recordIt == _providers.end())
    {
      AUDIO_LOG_ERROR("Player: No provider found for backend {}", backend);
      return;
    }

    // 4. Create the backend and swap it in the engine
    auto const& device = *it;
    auto newBackend = (*recordIt)->provider->createBackend(device, profile);
    _activeManager = (*recordIt)->provider.get();
    _playbackGeneration++;
    _engine->setBackend(std::move(newBackend), device);
  }

  void Player::pause()
  {
    _engine->pause();
  }

  void Player::resume()
  {
    _engine->resume();
  }

  void Player::stop()
  {
    _playbackGeneration++;
    _cachedRouteStatus = {};
    _cachedSystemGraph = {};
    _mergedGraph = {};
    _quality = Quality::Unknown;
    _qualityTooltip.clear();
    _graphSubscription.reset();
    _currentTrack.reset();
    _engine->stop();
  }

  void Player::seek(std::uint32_t positionMs)
  {
    _engine->seek(positionMs);
  }

  Player::Status Player::status() const
  {
    auto engineStatus = _engine->status();

    auto status = Player::Status{};
    status.engine = std::move(engineStatus);
    if (_currentTrack)
    {
      status.trackTitle = _currentTrack->title;
      status.trackArtist = _currentTrack->artist;
    }
    status.availableBackends = _cachedBackends;
    status.flow = _mergedGraph;
    status.isReady = isReady();

    // Inject controller-owned fields into the snapshot returned to the UI
    status.quality = _quality;
    status.qualityTooltip = _qualityTooltip;

    return status;
  }

  bool Player::isReady() const
  {
    auto const engineStatus = _engine->status();
    return engineStatus.backendId != kBackendNone && !_pendingOutput.has_value();
  }

  void Player::handleDevicesChanged(IBackendProvider* provider, std::vector<Device> const& devices)
  {
    // Update individual provider cache
    auto const it =
      std::ranges::find_if(_providers, [&](auto const& record) { return record->provider.get() == provider; });
    if (it != _providers.end())
    {
      (*it)->devices = devices;
    }

    // Rebuild global cache from all providers
    auto allDevices = std::vector<Device>{};
    auto snapshots = std::vector<IBackendProvider::Status>{};

    for (auto const& record : _providers)
    {
      auto status = record->provider->status();
      // The provider status might have its own internal device list, but we use the one from the subscription for
      // consistency
      status.devices = record->devices;
      snapshots.push_back(std::move(status));
      allDevices.insert(allDevices.end(), record->devices.begin(), record->devices.end());
    }

    _cachedBackends = std::move(snapshots);
    _allDevices = std::move(allDevices);

    // Keep the engine's current device capabilities up-to-date
    auto const currentSnap = _engine->status();
    auto const activeIt =
      std::ranges::find_if(_allDevices,
                           [&](Device const& dev)
                           { return dev.backendId == currentSnap.backendId && dev.id == currentSnap.currentDeviceId; });

    if (activeIt != _allDevices.end())
    {
      _engine->updateDevice(*activeIt);
    }

    if (_pendingOutput)
    {
      // Try to apply pending output
      auto const pending = *_pendingOutput;
      setOutput(pending.backend, pending.deviceId, pending.profile);

      if (!_pendingOutput)
      {
        AUDIO_LOG_INFO("Player: Pending output {}:{} ({}) successfully restored",
                       pending.backend,
                       pending.deviceId,
                       pending.profile);
      }
    }
  }

  void Player::handleRouteChanged(Engine::RouteStatus const& status, std::uint64_t generation)
  {
    if (generation != _playbackGeneration)
    {
      return;
    }

    _cachedRouteStatus = status;

    // Check if we have a valid anchor and manager to subscribe to the system graph
    if (_cachedRouteStatus.optAnchor && _activeManager != nullptr)
    {
      if (!_graphSubscription)
      {
        _graphSubscription = _activeManager->subscribeGraph(
          _cachedRouteStatus.optAnchor->id,
          [this, generation](flow::Graph const& graph)
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

  void Player::handleSystemGraphChanged(flow::Graph const& graph, std::uint64_t generation)
  {
    if (generation != _playbackGeneration)
    {
      return;
    }

    _cachedSystemGraph = graph;
    updateMergedGraph();
  }

  void Player::updateMergedGraph()
  {
    _mergedGraph = _cachedRouteStatus.flow;

    // Find the engine output format to enrich system nodes that might be missing it (like ALSA)
    auto optEngineFormat = std::optional<Format>{};

    for (auto const& node : _cachedRouteStatus.flow.nodes)
    {
      if (node.id == "rs-engine")
      {
        optEngineFormat = node.optFormat;
        break;
      }
    }

    // Add system graph nodes and links
    for (auto node : _cachedSystemGraph.nodes)
    {
      if (!node.optFormat && optEngineFormat)
      {
        node.optFormat = optEngineFormat;
      }

      _mergedGraph.nodes.push_back(node);
    }

    for (auto const& link : _cachedSystemGraph.connections)
    {
      _mergedGraph.connections.push_back(link);
    }

    // Find the backend stream node to bridge the engine to
    std::string streamNodeId;

    for (auto const& node : _cachedSystemGraph.nodes)
    {
      if (node.type == flow::NodeType::Stream)
      {
        streamNodeId = node.id;
        break;
      }
    }

    if (!streamNodeId.empty())
    {
      _mergedGraph.connections.push_back({.sourceId = "rs-engine", .destId = streamNodeId, .isActive = true});
    }

    analyzeAudioQuality();
  }

  std::vector<flow::Node const*> Player::findPlaybackPath(std::string const& startId) const
  {
    std::vector<flow::Node const*> path;
    auto currentId = startId;
    std::set<std::string> visited;

    while (!currentId.empty() && !visited.contains(currentId))
    {
      visited.insert(currentId);

      auto const it = std::ranges::find(_mergedGraph.nodes, currentId, &flow::Node::id);

      if (it == _mergedGraph.nodes.end())
      {
        break;
      }

      path.push_back(&(*it));

      std::string nextId;

      for (auto const& link : _mergedGraph.connections)
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

  void Player::processInputSources(flow::Node const& node,
                                   std::span<flow::Node const* const> path,
                                   std::unordered_map<std::string, std::set<std::string>> const& inputSources)
  {
    if (inputSources.contains(node.id))
    {
      auto const& sources = inputSources.at(node.id);
      std::vector<std::string> otherAppNames;

      for (auto const& srcId : sources)
      {
        bool const isInternal = std::ranges::contains(path, srcId, &flow::Node::id);

        if (!isInternal)
        {
          auto const it = std::ranges::find(_mergedGraph.nodes, srcId, &flow::Node::id);

          if (it != _mergedGraph.nodes.end())
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
        _quality = std::max(_quality, Quality::LinearIntervention);
      }
    }
  }

  void Player::assessNodeQuality(flow::Node const& node, flow::Node const* nextNode)
  {
    if (node.isLossySource)
    {
      appendLine(_qualityTooltip, std::format("• Source: Lossy format ({})", node.name));
      _quality = std::max(_quality, Quality::LossySource);
    }

    if (node.volumeNotUnity)
    {
      appendLine(_qualityTooltip, std::format("• Volume: Modification at {}", node.name));
      _quality = std::max(_quality, Quality::LinearIntervention);
    }

    if (node.isMuted)
    {
      appendLine(_qualityTooltip, std::format("• Status: {} is MUTED", node.name));
      _quality = std::max(_quality, Quality::LinearIntervention);
    }

    if (nextNode != nullptr)
    {
      if (node.optFormat && nextNode->optFormat)
      {
        auto const& f1 = *node.optFormat;
        auto const& f2 = *nextNode->optFormat;

        if (f1.sampleRate != f2.sampleRate)
        {
          appendLine(_qualityTooltip, std::format("• Resampling: {}Hz → {}Hz", f1.sampleRate, f2.sampleRate));
          _quality = std::max(_quality, Quality::LinearIntervention);
        }

        if (f1.channels != f2.channels)
        {
          appendLine(_qualityTooltip, std::format("• Channels: {}ch → {}ch", f1.channels, f2.channels));
          _quality = std::max(_quality, Quality::LinearIntervention);
        }
        else if (f1.bitDepth != f2.bitDepth || f1.isFloat != f2.isFloat)
        {
          if (isLosslessBitDepthChange(f1, f2))
          {
            appendLine(
              _qualityTooltip, f2.isFloat ? "• Bit-Transparent: Float mapping" : "• Bit-Transparent: Integer padding");
            _quality = std::max(_quality, f2.isFloat ? Quality::LosslessFloat : Quality::LosslessPadded);
          }
          else
          {
            appendLine(_qualityTooltip, std::format("• Precision: Truncated {}b → {}b", f1.bitDepth, f2.bitDepth));
            _quality = std::max(_quality, Quality::LinearIntervention);
          }
        }
      }
    }
  }

  void Player::analyzeAudioQuality()
  {
    // Now analyze the merged graph
    _quality = Quality::BitwisePerfect;
    _qualityTooltip.clear();

    if (_mergedGraph.nodes.empty())
    {
      _quality = Quality::Unknown;
      return;
    }

    appendLine(_qualityTooltip, "Audio Routing Analysis:");

    // 1. Build linear path
    auto const path = findPlaybackPath("rs-decoder");

    // 2. Identify mixing sources
    auto inputSources = std::unordered_map<std::string, std::set<std::string>>{};

    for (auto const& link : _mergedGraph.connections)
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

    if (_quality == Quality::BitwisePerfect)
    {
      appendLine(_qualityTooltip, "• Signal Path: Byte-perfect from decoder to device");
    }

    switch (_quality)
    {
      case Quality::BitwisePerfect: appendLine(_qualityTooltip, "\nConclusion: Bit-perfect output"); break;

      case Quality::LosslessPadded:
      case Quality::LosslessFloat: appendLine(_qualityTooltip, "\nConclusion: Lossless Conversion"); break;

      case Quality::LinearIntervention:
        appendLine(_qualityTooltip, "\nConclusion: Linear intervention (Resampled/Mixed/Vol)");
        break;

      case Quality::LossySource: appendLine(_qualityTooltip, "\nConclusion: Lossy source format"); break;

      case Quality::Clipped: appendLine(_qualityTooltip, "\nConclusion: Signal clipping detected"); break;

      case Quality::Unknown: break;
    }
  }
} // namespace ao::audio

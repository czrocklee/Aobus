// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Engine.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Player.h>
#include <ao/utility/IMainThreadDispatcher.h>
#include <ao/utility/Log.h>

#include <algorithm>
#include <atomic>
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

  struct Player::Impl final
  {
    struct ProviderRecord
    {
      std::unique_ptr<IBackendProvider> provider;
      Subscription subscription;
      std::vector<Device> devices;
    };

    struct PendingOutput
    {
      BackendId backend;
      DeviceId deviceId;
      ProfileId profile;
    };

    explicit Impl(std::shared_ptr<ao::IMainThreadDispatcher> dispatcher)
      : dispatcher{std::move(dispatcher)}
    {
    }

    std::uint64_t playbackGeneration = 1;
    std::vector<std::unique_ptr<ProviderRecord>> providers;
    std::optional<PendingOutput> pendingOutput;
    IBackendProvider* activeManager = nullptr;
    Subscription graphSubscription;

    std::shared_ptr<ao::IMainThreadDispatcher> dispatcher;
    std::unique_ptr<Engine> engine;

    mutable std::vector<IBackendProvider::Status> cachedBackends;
    mutable std::vector<Device> allDevices;

    Engine::RouteStatus cachedRouteStatus;
    flow::Graph cachedSystemGraph;
    flow::Graph mergedGraph;

    Quality quality = Quality::Unknown;
    std::string qualityTooltip;
    std::optional<TrackPlaybackDescriptor> currentTrack;

    std::function<void()> onTrackEnded;

    // Quality analysis helpers
    std::vector<flow::Node const*> findPlaybackPath(std::string const& startId) const;
    void assessNodeQuality(flow::Node const& node, flow::Node const* nextNode);
    void processInputSources(flow::Node const& node,
                             std::span<flow::Node const* const> path,
                             std::unordered_map<std::string, std::set<std::string>> const& inputSources);

    void handleDevicesChanged(Player* owner, IBackendProvider* provider, std::vector<Device> const& devices);
    void handleSystemGraphChanged(Player* owner, flow::Graph const& graph, std::uint64_t generation);
    void updateMergedGraph();
    void analyzeAudioQuality();
  };

  void Player::Impl::handleDevicesChanged(Player* owner, IBackendProvider* provider, std::vector<Device> const& devices)
  {
    // Update individual provider cache
    auto const it =
      std::ranges::find_if(providers, [&](auto const& record) { return record->provider.get() == provider; });
    if (it != providers.end())
    {
      (*it)->devices = devices;
    }

    // Rebuild global cache from all providers
    auto allDevicesList = std::vector<Device>{};
    auto snapshots = std::vector<IBackendProvider::Status>{};

    for (auto const& record : providers)
    {
      auto status = record->provider->status();
      // The provider status might have its own internal device list, but we use the one from the subscription for
      // consistency
      status.devices = record->devices;
      snapshots.push_back(std::move(status));
      allDevicesList.insert(allDevicesList.end(), record->devices.begin(), record->devices.end());
    }

    cachedBackends = std::move(snapshots);
    allDevices = std::move(allDevicesList);

    // Keep the engine's current device capabilities up-to-date
    auto const currentSnap = engine->status();
    auto const activeIt =
      std::ranges::find_if(allDevices,
                           [&](Device const& dev)
                           { return dev.backendId == currentSnap.backendId && dev.id == currentSnap.currentDeviceId; });

    if (activeIt != allDevices.end())
    {
      engine->updateDevice(*activeIt);
    }

    if (pendingOutput)
    {
      // Try to apply pending output
      auto const pending = *pendingOutput;
      owner->setOutput(pending.backend, pending.deviceId, pending.profile);

      if (!pendingOutput)
      {
        AUDIO_LOG_INFO("Player: Pending output {}:{} ({}) successfully restored",
                       pending.backend,
                       pending.deviceId,
                       pending.profile);
      }
    }
  }

  void Player::Impl::handleSystemGraphChanged(Player* /*owner*/, flow::Graph const& graph, std::uint64_t generation)
  {
    if (generation != playbackGeneration)
    {
      return;
    }

    cachedSystemGraph = graph;
    updateMergedGraph();
  }

  void Player::Impl::updateMergedGraph()
  {
    auto const& rs = cachedRouteStatus.state;

    mergedGraph = flow::Graph{
      .nodes =
        {
          flow::Node{.id = "ao-decoder",
                     .type = flow::NodeType::Decoder,
                     .name = "Decoder",
                     .optFormat = rs.decoderOutputFormat,
                     .isLossySource = rs.isLossySource},
          flow::Node{
            .id = "ao-engine", .type = flow::NodeType::Engine, .name = "Engine", .optFormat = rs.engineOutputFormat},
        },
      .connections =
        {
          flow::Connection{.sourceId = "ao-decoder", .destId = "ao-engine", .isActive = true},
        },
    };

    auto const optEngineFormat = rs.engineOutputFormat;

    for (auto node : cachedSystemGraph.nodes)
    {
      if (!node.optFormat && optEngineFormat.sampleRate > 0)
      {
        node.optFormat = optEngineFormat;
      }

      mergedGraph.nodes.push_back(node);
    }

    for (auto const& link : cachedSystemGraph.connections)
    {
      mergedGraph.connections.push_back(link);
    }

    auto streamNodeId = std::string{};

    for (auto const& node : cachedSystemGraph.nodes)
    {
      if (node.type == flow::NodeType::Stream)
      {
        streamNodeId = node.id;
        break;
      }
    }

    if (!streamNodeId.empty())
    {
      mergedGraph.connections.push_back({.sourceId = "ao-engine", .destId = streamNodeId, .isActive = true});
    }

    analyzeAudioQuality();
  }

  std::vector<flow::Node const*> Player::Impl::findPlaybackPath(std::string const& startId) const
  {
    std::vector<flow::Node const*> path;
    auto currentId = std::string_view{startId};
    std::set<std::string_view> visited;

    while (!currentId.empty() && !visited.contains(currentId))
    {
      visited.insert(currentId);

      auto const it = std::ranges::find(mergedGraph.nodes, currentId, &flow::Node::id);

      if (it == mergedGraph.nodes.end())
      {
        break;
      }

      path.push_back(&(*it));

      auto nextId = std::string_view{};

      for (auto const& link : mergedGraph.connections)
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

  void Player::Impl::processInputSources(flow::Node const& node,
                                         std::span<flow::Node const* const> path,
                                         std::unordered_map<std::string, std::set<std::string>> const& inputSources)
  {
    if (inputSources.contains(node.id))
    {
      auto const& sources = inputSources.at(node.id);
      auto otherAppNames = std::vector<std::string>{};

      for (auto const& srcId : sources)
      {
        bool const isInternal = std::ranges::contains(path, srcId, &flow::Node::id);

        if (!isInternal)
        {
          auto const it = std::ranges::find(mergedGraph.nodes, srcId, &flow::Node::id);

          if (it != mergedGraph.nodes.end())
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
        auto apps = std::string{};

        for (size_t j = 0; j < otherAppNames.size(); ++j)
        {
          apps += otherAppNames[j];

          if (j < otherAppNames.size() - 1)
          {
            apps += ", ";
          }
        }

        appendLine(qualityTooltip, std::format("• Mixed: {} shared with {}", node.name, apps));
        quality = std::max(quality, Quality::LinearIntervention);
      }
    }
  }

  void Player::Impl::assessNodeQuality(flow::Node const& node, flow::Node const* nextNode)
  {
    if (node.isLossySource)
    {
      appendLine(qualityTooltip, std::format("• Source: Lossy format ({})", node.name));
      quality = std::max(quality, Quality::LossySource);
    }

    if (node.volumeNotUnity)
    {
      appendLine(qualityTooltip, std::format("• Volume: Modification at {}", node.name));
      quality = std::max(quality, Quality::LinearIntervention);
    }

    if (node.isMuted)
    {
      appendLine(qualityTooltip, std::format("• Status: {} is MUTED", node.name));
      quality = std::max(quality, Quality::LinearIntervention);
    }

    if (nextNode != nullptr)
    {
      if (node.optFormat && nextNode->optFormat)
      {
        auto const& f1 = *node.optFormat;
        auto const& f2 = *nextNode->optFormat;

        if (f1.sampleRate != f2.sampleRate)
        {
          appendLine(qualityTooltip, std::format("• Resampling: {}Hz → {}Hz", f1.sampleRate, f2.sampleRate));
          quality = std::max(quality, Quality::LinearIntervention);
        }

        if (f1.channels != f2.channels)
        {
          appendLine(qualityTooltip, std::format("• Channels: {}ch → {}ch", f1.channels, f2.channels));
          quality = std::max(quality, Quality::LinearIntervention);
        }
        else if (f1.bitDepth != f2.bitDepth || f1.isFloat != f2.isFloat)
        {
          if (isLosslessBitDepthChange(f1, f2))
          {
            appendLine(
              qualityTooltip, f2.isFloat ? "• Bit-Transparent: Float mapping" : "• Bit-Transparent: Integer padding");
            quality = std::max(quality, f2.isFloat ? Quality::LosslessFloat : Quality::LosslessPadded);
          }
          else
          {
            appendLine(qualityTooltip, std::format("• Precision: Truncated {}b → {}b", f1.bitDepth, f2.bitDepth));
            quality = std::max(quality, Quality::LinearIntervention);
          }
        }
      }
    }
  }

  void Player::Impl::analyzeAudioQuality()
  {
    // Now analyze the merged graph
    quality = Quality::BitwisePerfect;
    qualityTooltip.clear();

    if (mergedGraph.nodes.empty())
    {
      quality = Quality::Unknown;
      return;
    }

    appendLine(qualityTooltip, "Audio Routing Analysis:");

    // 1. Build linear path
    auto const path = findPlaybackPath("ao-decoder");

    // 2. Identify mixing sources
    auto inputSources = std::unordered_map<std::string, std::set<std::string>>{};

    for (auto const& link : mergedGraph.connections)
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

    if (quality == Quality::BitwisePerfect)
    {
      appendLine(qualityTooltip, "• Signal Path: Byte-perfect from decoder to device");
    }

    switch (quality)
    {
      case Quality::BitwisePerfect:
      case Quality::LosslessPadded: appendLine(qualityTooltip, "\nConclusion: Bit-perfect output"); break;

      case Quality::LosslessFloat: appendLine(qualityTooltip, "\nConclusion: Lossless Conversion"); break;

      case Quality::LinearIntervention:
        appendLine(qualityTooltip, "\nConclusion: Linear intervention (Resampled/Mixed/Vol)");
        break;

      case Quality::LossySource: appendLine(qualityTooltip, "\nConclusion: Lossy source format"); break;

      case Quality::Clipped: appendLine(qualityTooltip, "\nConclusion: Signal clipping detected"); break;

      case Quality::Unknown: break;
    }
  }

  Player::Player(std::shared_ptr<ao::IMainThreadDispatcher> dispatcher)
    : _impl{std::make_unique<Impl>(std::move(dispatcher))}
  {
    // Start with a NullBackend until a provider provides something real
    _impl->engine = std::make_unique<Engine>(std::make_unique<NullBackend>(),
                                             Device{.id = DeviceId{"null"},
                                                    .displayName = "None",
                                                    .description = "No audio output selected",
                                                    .backendId = kBackendNone,
                                                    .capabilities = {}},
                                             _impl->dispatcher);

    _impl->engine->setOnTrackEnded(
      [this]()
      {
        if (_impl->onTrackEnded)
        {
          _impl->onTrackEnded();
        }
      });

    _impl->engine->setOnRouteChanged(
      [this](Engine::RouteStatus const& status)
      {
        // Capture generation to prevent stale updates
        auto const generation = _impl->playbackGeneration;

        if (_impl->dispatcher)
        {
          _impl->dispatcher->dispatch([this, status, generation]() { handleRouteChanged(status, generation); });
        }
        else
        {
          handleRouteChanged(status, generation);
        }
      });
  }

  void Player::setTrackEndedCallback(std::function<void()> callback)
  {
    _impl->onTrackEnded = std::move(callback);
  }

  Player::~Player()
  {
    _impl->engine->setOnRouteChanged(nullptr);
  }

  void Player::addProvider(std::unique_ptr<IBackendProvider> provider)
  {
    if (!provider)
    {
      return;
    }

    auto record = std::make_unique<Impl::ProviderRecord>();
    record->provider = std::move(provider);

    auto* const providerPtr = record->provider.get();
    auto* const recordPtr = record.get();

    _impl->providers.push_back(std::move(record));

    recordPtr->subscription = providerPtr->subscribeDevices(
      [this, providerPtr, recordPtr](std::vector<Device> const& devices)
      {
        if (_impl->dispatcher)
        {
          _impl->dispatcher->dispatch(
            [this, providerPtr, recordPtr, devices]()
            {
              recordPtr->devices = devices;
              _impl->handleDevicesChanged(this, providerPtr, devices);
            });
        }
        else
        {
          recordPtr->devices = devices;
          _impl->handleDevicesChanged(this, providerPtr, devices);
        }

        return true;
      });
  }

  void Player::play(TrackPlaybackDescriptor const& descriptor)
  {
    if (!isReady())
    {
      AUDIO_LOG_WARN("Player: Playback ignored because audio backend is not ready (pending discovery)");
      return;
    }

    _impl->playbackGeneration++;
    _impl->cachedRouteStatus = {};
    _impl->cachedSystemGraph = {};
    _impl->mergedGraph = {};
    _impl->quality = Quality::Unknown;
    _impl->qualityTooltip.clear();
    _impl->graphSubscription.reset();
    _impl->currentTrack = descriptor;
    _impl->engine->play(descriptor);
  }

  void Player::setOutput(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile)
  {
    auto const currentSnap = _impl->engine->status();

    // 1. Check if we already have this output active
    if (backend == currentSnap.backendId && profile == currentSnap.profileId && deviceId == currentSnap.currentDeviceId)
    {
      _impl->pendingOutput.reset();
      return;
    }

    // 2. Find the Device matching the kind and id from our cache
    auto const it = std::ranges::find_if(
      _impl->allDevices, [&](Device const& dev) { return dev.backendId == backend && dev.id == deviceId; });

    if (it == _impl->allDevices.end())
    {
      // If we don't have it yet, store it as pending.
      _impl->pendingOutput = Impl::PendingOutput{.backend = backend, .deviceId = deviceId, .profile = profile};
      AUDIO_LOG_DEBUG("Player: Requested output {}:{} not yet available, pending discovery", backend, deviceId);
      return;
    }

    // Found it! Clear any pending output.
    _impl->pendingOutput.reset();

    // 3. Find the provider object that can handle this BackendId
    auto const recordIt = std::ranges::find_if(
      _impl->providers, [&](auto const& record) { return record->provider->status().metadata.id == backend; });

    if (recordIt == _impl->providers.end())
    {
      AUDIO_LOG_ERROR("Player: No provider found for backend {}", backend);
      return;
    }

    // 4. Create the backend and swap it in the engine
    auto const& device = *it;
    auto newBackend = (*recordIt)->provider->createBackend(device, profile);
    _impl->activeManager = (*recordIt)->provider.get();
    _impl->playbackGeneration++;
    _impl->engine->setBackend(std::move(newBackend), device);
  }

  void Player::pause()
  {
    _impl->engine->pause();
  }

  void Player::resume()
  {
    _impl->engine->resume();
  }

  void Player::stop()
  {
    _impl->playbackGeneration++;
    _impl->cachedRouteStatus = {};
    _impl->cachedSystemGraph = {};
    _impl->mergedGraph = {};
    _impl->quality = Quality::Unknown;
    _impl->qualityTooltip.clear();
    _impl->graphSubscription.reset();
    _impl->currentTrack.reset();
    _impl->engine->stop();
  }

  void Player::seek(std::uint32_t positionMs)
  {
    _impl->engine->seek(positionMs);
  }

  void Player::setVolume(float vol)
  {
    _impl->engine->setVolume(vol);
  }

  void Player::setMuted(bool muted)
  {
    _impl->engine->setMuted(muted);
  }

  void Player::toggleMute()
  {
    auto const engineStatus = _impl->engine->status();
    setMuted(!engineStatus.muted);
  }

  Player::Status Player::status() const
  {
    auto status = Player::Status{};
    status.engine = _impl->engine->status();

    if (_impl->currentTrack)
    {
      status.trackTitle = _impl->currentTrack->title;
      status.trackArtist = _impl->currentTrack->artist;
    }

    status.availableBackends = _impl->cachedBackends;
    status.flow = _impl->mergedGraph;
    status.isReady = isReady();

    status.volume = status.engine.volume;
    status.muted = status.engine.muted;
    status.volumeAvailable = status.engine.volumeAvailable;

    status.quality = _impl->quality;
    status.qualityTooltip = _impl->qualityTooltip;

    return status;
  }

  Transport Player::transport() const
  {
    return _impl->engine->transport();
  }

  bool Player::isReady() const
  {
    return _impl->engine->backendId() != kBackendNone && !_impl->pendingOutput.has_value();
  }

  void Player::handleRouteChanged(Engine::RouteStatus const& status, std::uint64_t generation)
  {
    if (generation != _impl->playbackGeneration)
    {
      return;
    }

    _impl->cachedRouteStatus = status;

    // Check if we have a valid anchor and manager to subscribe to the system graph
    if (_impl->cachedRouteStatus.optAnchor && _impl->activeManager != nullptr)
    {
      if (!_impl->graphSubscription)
      {
        _impl->graphSubscription = _impl->activeManager->subscribeGraph(
          _impl->cachedRouteStatus.optAnchor->id,
          [this, generation](flow::Graph const& graph)
          {
            if (_impl->dispatcher)
            {
              _impl->dispatcher->dispatch([this, graph, generation]()
                                          { _impl->handleSystemGraphChanged(this, graph, generation); });
            }
            else
            {
              _impl->handleSystemGraphChanged(this, graph, generation);
            }
          });
      }
    }
    else
    {
      _impl->graphSubscription.reset();
      _impl->cachedSystemGraph = {};
    }

    _impl->updateMergedGraph();
  }

  std::uint64_t Player::playbackGeneration() const noexcept
  {
    return _impl->playbackGeneration;
  }
} // namespace ao::audio

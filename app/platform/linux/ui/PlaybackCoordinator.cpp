// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#include "platform/linux/ui/PlaybackCoordinator.h"
#include <rs/utility/Log.h>

#ifdef ALSA_FOUND
#include "platform/linux/playback/AlsaManager.h"
#endif
#ifdef PIPEWIRE_FOUND
#include "platform/linux/playback/PipeWireManager.h"
#endif

namespace app::ui
{
  PlaybackCoordinator::PlaybackCoordinator(IPlaybackHost& host,
                                           std::shared_ptr<GtkMainThreadDispatcher> dispatcher,
                                           std::function<TrackRowDataProvider*()> providerSource)
    : _host(host)
    , _providerSource(std::move(providerSource))
    , _playbackBar(std::make_unique<PlaybackBar>())
    , _dispatcher(std::move(dispatcher))
  {
    setupPlayback();
  }

  PlaybackCoordinator::~PlaybackCoordinator()
  {
    if (_playbackTimer != 0)
    {
      g_source_remove(_playbackTimer);
      _playbackTimer = 0;
    }
  }

  void PlaybackCoordinator::setPlaybackController(std::unique_ptr<rs::audio::PlaybackController> controller)
  {
    _playbackController = std::move(controller);
    if (_playbackController)
    {
      _playbackController->setTrackEndedCallback([this]() { handlePlaybackFinished(); });
    }
  }

  void PlaybackCoordinator::setupPlayback()
  {
    auto controller = std::make_unique<rs::audio::PlaybackController>(_dispatcher);

#ifdef PIPEWIRE_FOUND
    controller->addManager(std::make_unique<app::playback::PipeWireManager>());
#endif
#ifdef ALSA_FOUND
    controller->addManager(std::make_unique<app::playback::AlsaManager>());
#endif

    setPlaybackController(std::move(controller));

    _playbackBar->signalPlayRequested().connect(sigc::mem_fun(*this, &PlaybackCoordinator::onPlayRequested));
    _playbackBar->signalPauseRequested().connect(sigc::mem_fun(*this, &PlaybackCoordinator::onPauseRequested));
    _playbackBar->signalStopRequested().connect(sigc::mem_fun(*this, &PlaybackCoordinator::onStopRequested));
    _playbackBar->signalSeekRequested().connect(sigc::mem_fun(*this, &PlaybackCoordinator::onSeekRequested));

    _playbackTimer = g_timeout_add(
      100,
      [](void* data) -> int
      {
        auto* self = static_cast<PlaybackCoordinator*>(data);
        self->refreshPlaybackBar();
        return true;
      },
      this);
  }

  void PlaybackCoordinator::refreshPlaybackBar()
  {
    if (!_playbackController)
    {
      return;
    }

    auto snapshot = _playbackController->snapshot();
    _lastPlaybackState = snapshot.state;
    _host.updatePlaybackStatus(snapshot);

    if (snapshot.statusText != _lastPlaybackErrorMessage)
    {
      _lastPlaybackErrorMessage = snapshot.statusText;

      if (!snapshot.statusText.empty() && snapshot.state == rs::audio::TransportState::Error)
      {
        _host.showPlaybackMessage(snapshot.statusText, std::chrono::seconds{10});
      }
    }

    _playbackBar->setSnapshot(snapshot);
  }

  void PlaybackCoordinator::onPlayRequested()
  {
    playCurrentSelection();
  }

  void PlaybackCoordinator::onPauseRequested()
  {
    pausePlayback();
  }

  void PlaybackCoordinator::onStopRequested()
  {
    stopPlayback();
  }

  void PlaybackCoordinator::onSeekRequested(std::uint32_t positionMs)
  {
    seekPlayback(positionMs);
  }

  void PlaybackCoordinator::playCurrentSelection()
  {
    if (!_playbackController)
    {
      return;
    }

    auto const snapshot = _playbackController->snapshot();

    if (snapshot.state == rs::audio::TransportState::Paused)
    {
      _playbackController->resume();
      return;
    }

    auto const* ctx = _host.currentVisibleTrackPageContext();

    if (ctx != nullptr && ctx->page != nullptr)
    {
      if (auto const trackId = ctx->page->getPrimarySelectedTrackId(); trackId)
      {
        startPlaybackFromVisiblePage(*ctx->page, *trackId);
      }
    }
  }

  void PlaybackCoordinator::pausePlayback()
  {
    if (_playbackController)
    {
      _playbackController->pause();
    }
  }

  void PlaybackCoordinator::stopPlayback()
  {
    if (_playbackController)
    {
      _playbackController->stop();
      clearActivePlaybackSequence();
      _lastPlaybackState = rs::audio::TransportState::Idle;
    }
  }

  void PlaybackCoordinator::seekPlayback(std::uint32_t positionMs)
  {
    if (_playbackController)
    {
      _playbackController->seek(positionMs);
    }
  }

  bool PlaybackCoordinator::startPlaybackFromVisiblePage(TrackViewPage const& page, rs::TrackId trackId)
  {
    auto trackIds = page.getVisibleTrackIds();
    return startPlaybackSequence(std::move(trackIds), trackId, page.getListId());
  }

  bool PlaybackCoordinator::startPlaybackSequence(std::vector<rs::TrackId> trackIds,
                                                  rs::TrackId startTrackId,
                                                  std::optional<rs::ListId> sourceListId)
  {
    if (!_playbackController)
    {
      return false;
    }

    auto const it = std::ranges::find(trackIds, startTrackId);
    auto startIndex = std::size_t{0};

    if (it == trackIds.end())
    {
      trackIds = {startTrackId};
    }
    else
    {
      startIndex = static_cast<std::size_t>(std::distance(trackIds.begin(), it));
    }

    _activePlaybackSequence = ActivePlaybackSequence{
      .trackIds = std::move(trackIds),
      .currentIndex = startIndex,
      .sourceListId = sourceListId,
    };

    if (playTrackAtSequenceIndex(startIndex))
    {
      return true;
    }

    clearActivePlaybackSequence();
    return false;
  }

  bool PlaybackCoordinator::playTrackAtSequenceIndex(std::size_t index)
  {
    if (!_playbackController || !_activePlaybackSequence)
    {
      return false;
    }

    auto* rowDataProvider = _providerSource();
    if (!rowDataProvider)
    {
      return false;
    }

    auto& sequence = *_activePlaybackSequence;

    for (auto i = index; i < sequence.trackIds.size(); ++i)
    {
      if (auto descriptor = rowDataProvider->getPlaybackDescriptor(sequence.trackIds[i]))
      {
        sequence.currentIndex = i;
        _playbackController->play(*descriptor);
        return true;
      }
    }

    return false;
  }

  void PlaybackCoordinator::jumpToPlayingList()
  {
    if (!_activePlaybackSequence || !_activePlaybackSequence->sourceListId)
    {
      return;
    }

    auto const listId = *_activePlaybackSequence->sourceListId;
    auto const trackId = _activePlaybackSequence->trackIds[_activePlaybackSequence->currentIndex];

    _host.showListPage(listId);

    if (auto* ctx = _host.findTrackPageContext(listId); ctx)
    {
      ctx->page->selectTrack(trackId);
    }
  }

  void PlaybackCoordinator::handlePlaybackFinished()
  {
    if (!_activePlaybackSequence)
    {
      return;
    }

    auto const nextIndex = _activePlaybackSequence->currentIndex + 1;

    if (playTrackAtSequenceIndex(nextIndex))
    {
      return;
    }

    clearActivePlaybackSequence();

    if (_playbackController)
    {
      _lastPlaybackState = rs::audio::TransportState::Idle;
      _playbackController->stop();
    }
  }

  void PlaybackCoordinator::clearActivePlaybackSequence()
  {
    _activePlaybackSequence.reset();
  }

} // namespace app::ui

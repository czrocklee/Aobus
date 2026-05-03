// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackCoordinator.h"
#include <ao/utility/Log.h>

#ifdef ALSA_FOUND
#include <ao/audio/backend/AlsaProvider.h>
#endif
#ifdef PIPEWIRE_FOUND
#include <ao/audio/backend/PipeWireProvider.h>
#endif

namespace ao::gtk
{
  PlaybackCoordinator::PlaybackCoordinator(IPlaybackHost& host,
                                           std::shared_ptr<GtkMainThreadDispatcher> dispatcher,
                                           std::function<TrackRowDataProvider*()> providerSource)
    : _host{host}
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

  void PlaybackCoordinator::setPlaybackController(std::unique_ptr<ao::audio::Player> controller)
  {
    _player = std::move(controller);
    if (_player)
    {
      _player->setTrackEndedCallback([this]() { handlePlaybackFinished(); });
    }
  }

  void PlaybackCoordinator::setupPlayback()
  {
    auto controller = std::make_unique<ao::audio::Player>(_dispatcher);

#ifdef PIPEWIRE_FOUND
    controller->addProvider(std::make_unique<ao::audio::backend::PipeWireProvider>());
#endif
#ifdef ALSA_FOUND
    controller->addProvider(std::make_unique<ao::audio::backend::AlsaProvider>());
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
    if (!_player)
    {
      return;
    }

    auto status = _player->status();
    _lastPlaybackState = status.engine.transport;
    _host.updatePlaybackStatus(status);

    if (status.engine.statusText != _lastPlaybackErrorMessage)
    {
      _lastPlaybackErrorMessage = status.engine.statusText;

      if (!status.engine.statusText.empty() && status.engine.transport == ao::audio::Transport::Error)
      {
        static constexpr auto errorDisplayDuration = std::chrono::seconds{10};
        _host.showPlaybackMessage(status.engine.statusText, errorDisplayDuration);
      }
    }

    _playbackBar->setSnapshot(status);
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
    if (!_player)
    {
      return;
    }

    auto const status = _player->status();

    if (status.engine.transport == ao::audio::Transport::Paused)
    {
      _player->resume();
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
    if (_player != nullptr)
    {
      _player->pause();
    }
  }

  void PlaybackCoordinator::stopPlayback()
  {
    if (_player != nullptr)
    {
      _player->stop();
      clearActivePlaybackSequence();
      _lastPlaybackState = ao::audio::Transport::Idle;
    }
  }

  void PlaybackCoordinator::seekPlayback(std::uint32_t positionMs)
  {
    if (_player != nullptr)
    {
      _player->seek(positionMs);
    }
  }

  bool PlaybackCoordinator::startPlaybackFromVisiblePage(TrackViewPage const& page, ao::TrackId trackId)
  {
    auto trackIds = page.getVisibleTrackIds();
    return startPlaybackSequence(std::move(trackIds), trackId, page.getListId());
  }

  bool PlaybackCoordinator::startPlaybackSequence(std::vector<ao::TrackId> trackIds,
                                                  ao::TrackId startTrackId,
                                                  std::optional<ao::ListId> sourceListId)
  {
    if (!_player)
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
    if (_player == nullptr || !_activePlaybackSequence)
    {
      return false;
    }

    auto* const rowDataProvider = _providerSource();

    if (rowDataProvider == nullptr)
    {
      return false;
    }

    auto& sequence = *_activePlaybackSequence;

    for (auto i = index; i < sequence.trackIds.size(); ++i)
    {
      if (auto descriptor = rowDataProvider->getPlaybackDescriptor(sequence.trackIds[i]))
      {
        sequence.currentIndex = i;
        _player->play(*descriptor);
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

    if (_player)
    {
      _lastPlaybackState = ao::audio::Transport::Idle;
      _player->stop();
    }
  }

  void PlaybackCoordinator::clearActivePlaybackSequence()
  {
    _activePlaybackSequence.reset();
  }
} // namespace ao::gtk

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/SmtcBridge.h"

#include "platform/MemoryRandomAccessStream.h"
#include <ao/CoreIds.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/library/track/CoverArtRequestModel.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <stop_token>
#include <utility>

namespace ao::winui
{
  namespace
  {
    winrt::Windows::Media::MediaPlaybackStatus playbackStatus(audio::Transport const transport) noexcept
    {
      using Status = winrt::Windows::Media::MediaPlaybackStatus;

      switch (transport)
      {
        case audio::Transport::Playing:
        case audio::Transport::Opening:
        case audio::Transport::Buffering:
        case audio::Transport::Seeking: return Status::Playing;
        case audio::Transport::Paused: return Status::Paused;
        case audio::Transport::Stopping:
        case audio::Transport::Idle: return Status::Stopped;
        case audio::Transport::Error: return Status::Closed;
      }

      return Status::Closed;
    }

    uimodel::PlaybackCommand commandForButton(winrt::Windows::Media::SystemMediaTransportControlsButton const button)
    {
      using Button = winrt::Windows::Media::SystemMediaTransportControlsButton;

      switch (button)
      {
        case Button::Play: return uimodel::PlaybackCommand::Play;
        case Button::Pause: return uimodel::PlaybackCommand::Pause;
        case Button::Stop: return uimodel::PlaybackCommand::Stop;
        case Button::Next: return uimodel::PlaybackCommand::Next;
        case Button::Previous: return uimodel::PlaybackCommand::Previous;
        default: return uimodel::PlaybackCommand::PlayPause;
      }
    }
  } // namespace

  struct SmtcBridge::State final
  {
    winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher{nullptr};
    winrt::Windows::Media::SystemMediaTransportControls controls{nullptr};
    uimodel::PlaybackCommandSurface* commands = nullptr;
    uimodel::CoverArtRequestModel artwork{};
    ResourceId displayedArtworkId{kInvalidResourceId};
    winrt::event_token buttonToken{};
    bool hasButtonToken = false;
    bool active = false;
  };

  SmtcBridge::SmtcBridge(HWND const window, winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher)
    : _statePtr{std::make_shared<State>()}
  {
    _statePtr->dispatcher = std::move(dispatcher);
    auto interop = winrt::get_activation_factory<winrt::Windows::Media::SystemMediaTransportControls,
                                                 ::ISystemMediaTransportControlsInterop>();
    winrt::check_hresult(interop->GetForWindow(window,
                                               winrt::guid_of<winrt::Windows::Media::SystemMediaTransportControls>(),
                                               winrt::put_abi(_statePtr->controls)));

    auto const state = std::weak_ptr<State>{_statePtr};
    _statePtr->buttonToken = _statePtr->controls.ButtonPressed(
      [state](winrt::Windows::Media::SystemMediaTransportControls const&,
              winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs const& args)
      {
        auto const command = commandForButton(args.Button());
        if (auto statePtr = state.lock())
        {
          statePtr->dispatcher.TryEnqueue(
            [state, command]
            {
              if (auto statePtr = state.lock(); statePtr && statePtr->active && statePtr->commands != nullptr)
              {
                statePtr->commands->execute(command);
              }
            });
        }
      });
    _statePtr->hasButtonToken = true;
  }

  SmtcBridge::~SmtcBridge()
  {
    unbind();

    if (_statePtr->hasButtonToken)
    {
      _statePtr->controls.ButtonPressed(_statePtr->buttonToken);
      _statePtr->hasButtonToken = false;
    }
  }

  void SmtcBridge::bind(std::shared_ptr<rt::AppRuntime> runtimePtr, uimodel::PlaybackCommandSurface& commands)
  {
    unbind();
    _runtimePtr = std::move(runtimePtr);
    _statePtr->active = true;
    _statePtr->commands = &commands;
    _statePtr->controls.IsEnabled(true);
    _statePtr->controls.IsPlayEnabled(true);
    _statePtr->controls.IsPauseEnabled(true);
    _statePtr->controls.IsStopEnabled(true);
    _statePtr->controls.IsNextEnabled(true);
    _statePtr->controls.IsPreviousEnabled(true);
    _snapshotSub = _runtimePtr->playback().events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) noexcept
                                                               { handleSnapshot(snapshot); });
    handleSnapshot(_runtimePtr->playback().snapshot());
  }

  void SmtcBridge::unbind()
  {
    _artworkTask.reset();
    _snapshotSub.reset();
    _statePtr->active = false;
    _statePtr->commands = nullptr;
    _statePtr->displayedArtworkId = kInvalidResourceId;
    _statePtr->artwork.reset();
    auto updater = _statePtr->controls.DisplayUpdater();
    updater.Thumbnail(nullptr);
    updater.Update();
    _statePtr->controls.IsEnabled(false);
    _runtimePtr.reset();
  }

  void SmtcBridge::handleSnapshot(rt::PlaybackSnapshot const& snapshot)
  {
    auto const& transport = snapshot.transport;
    _statePtr->controls.PlaybackStatus(playbackStatus(transport.transport));
    auto updater = _statePtr->controls.DisplayUpdater();
    updater.Type(winrt::Windows::Media::MediaPlaybackType::Music);
    auto music = updater.MusicProperties();
    music.Title(winrt::to_hstring(transport.nowPlaying.title));
    music.Artist(winrt::to_hstring(transport.nowPlaying.artist));
    music.AlbumTitle(winrt::to_hstring(transport.nowPlaying.album));
    updater.Update();

    if (transport.nowPlaying.coverArtId == _statePtr->displayedArtworkId)
    {
      return;
    }

    _statePtr->displayedArtworkId = transport.nowPlaying.coverArtId;
    updater.Thumbnail(nullptr);
    updater.Update();

    if (transport.nowPlaying.coverArtId == kInvalidResourceId)
    {
      _statePtr->artwork.clearSelection();
    }
    else
    {
      updateArtwork(transport.nowPlaying.coverArtId);
    }
  }

  void SmtcBridge::updateArtwork(ResourceId const resourceId)
  {
    _artworkTask.reset();
    auto const token = _statePtr->artwork.select(resourceId);
    auto const state = std::weak_ptr<State>{_statePtr};
    auto* const tasks = &_runtimePtr->library().taskService();
    auto* const runtime = &_runtimePtr->async();
    // The runtime and task service outlive the bridge task. Bridge-owned UI
    // state is touched only after the cancellation-checked callback hop.
    _artworkTask =
      runtime->spawnCancellable([state, tasks, runtime, token](std::stop_token const stopToken) mutable
                                { return updateArtworkWorkflow(state, tasks, runtime, token, stopToken); });
  }

  async::Task<void> SmtcBridge::updateArtworkWorkflow(std::weak_ptr<State> const state,
                                                      rt::LibraryTaskService* const tasks,
                                                      async::Runtime* const runtime,
                                                      uimodel::CoverArtRequestToken const token,
                                                      std::stop_token const stopToken)
  {
    try
    {
      auto bytes = co_await tasks->loadResourceAsync(token.resourceId, stopToken);

      if (!bytes || !*bytes)
      {
        co_return;
      }

      auto payload = std::move(**bytes);
      co_await runtime->resumeOnCallbackExecutor(stopToken);
      auto statePtr = state.lock();
      if (!statePtr || !statePtr->active)
      {
        co_return;
      }
      if (!statePtr->artwork.store(token, std::move(payload)))
      {
        co_return;
      }

      writeArtworkStream(*statePtr, token);
      statePtr.reset();
    }
    catch (async::OperationCancelled const&)
    {
    }
    catch (...)
    {
      runtime->reportUnhandledException(std::current_exception(), "Windows SMTC cover-art workflow");
    }
  }

  void SmtcBridge::writeArtworkStream(State& state, uimodel::CoverArtRequestToken const token)
  {
    try
    {
      auto const cached = state.artwork.cached(token.resourceId);
      auto stream = makeMemoryRandomAccessStream(cached);
      if (!state.active || !state.artwork.accepts(token))
      {
        return;
      }

      auto updater = state.controls.DisplayUpdater();
      updater.Thumbnail(winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromStream(stream));
      updater.Update();
    }
    catch (winrt::hresult_error const& error)
    {
      APP_LOG_WARN("Windows SMTC cover-art stream failed: {}", winrt::to_string(error.message()));
    }
  }
} // namespace ao::winui

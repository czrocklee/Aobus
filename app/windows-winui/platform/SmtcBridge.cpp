// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/SmtcBridge.h"

#include <ao/CoreIds.h>
#include <ao/async/Task.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/winui/MemoryRandomAccessStream.h>
#include <ao/winui/WinUiErrorBoundary.h>

#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>

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
    ResourceId displayedArtworkId{kInvalidResourceId};
    winrt::Windows::Media::SystemMediaTransportControls::ButtonPressed_revoker buttonRevoker{};
    bool active = false;
  };

  SmtcBridge::SmtcBridge(HWND window, winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher)
    : _statePtr{std::make_shared<State>()}
  {
    _statePtr->dispatcher = std::move(dispatcher);
    auto interop = winrt::get_activation_factory<winrt::Windows::Media::SystemMediaTransportControls,
                                                 ::ISystemMediaTransportControlsInterop>();
    winrt::check_hresult(interop->GetForWindow(window,
                                               winrt::guid_of<winrt::Windows::Media::SystemMediaTransportControls>(),
                                               winrt::put_abi(_statePtr->controls)));

    auto const weakStatePtr = std::weak_ptr<State>{_statePtr};
    _statePtr->buttonRevoker = _statePtr->controls.ButtonPressed(
      winrt::auto_revoke,
      [weakStatePtr](winrt::Windows::Media::SystemMediaTransportControls const&,
                     winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs const& args)
      {
        auto const command = commandForButton(args.Button());

        if (auto statePtr = weakStatePtr.lock(); statePtr)
        {
          statePtr->dispatcher.TryEnqueue(
            [weakStatePtr, command]
            {
              if (auto statePtr = weakStatePtr.lock(); statePtr && statePtr->active && statePtr->commands != nullptr)
              {
                statePtr->commands->execute(command);
              }
            });
        }
      });
  }

  SmtcBridge::~SmtcBridge()
  {
    unbind();
    _statePtr->buttonRevoker.revoke();
  }

  void SmtcBridge::bind(rt::AppRuntime& runtime,
                        uimodel::PlaybackCommandSurface& commands,
                        rt::ResourceByteLoader& resourceBytes)
  {
    unbind();
    _runtime = &runtime;
    _resourceBytes = &resourceBytes;
    _statePtr->active = true;
    _statePtr->commands = &commands;
    _statePtr->controls.IsEnabled(true);
    _statePtr->controls.IsPlayEnabled(true);
    _statePtr->controls.IsPauseEnabled(true);
    _statePtr->controls.IsStopEnabled(true);
    _statePtr->controls.IsNextEnabled(true);
    _statePtr->controls.IsPreviousEnabled(true);
    _snapshotSub = runtime.playback().events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot)
                                                          { handleSnapshot(snapshot); });
    handleSnapshot(runtime.playback().snapshot());
  }

  void SmtcBridge::unbind() noexcept
  {
    // Close admission before cancellation so queued button/artwork continuations
    // become harmless even if native revocation is delayed.
    _statePtr->active = false;
    _statePtr->commands = nullptr;
    _statePtr->displayedArtworkId = kInvalidResourceId;
    _resourceBytes = nullptr;
    _runtime = nullptr;

    _artworkRequest.reset();
    _artworkTask.reset();
    _snapshotSub.reset();

    // Unlike an in-window widget, the system transport overlay outlives this
    // window. Leaving it enabled with a stale thumbnail is visible to the user,
    // so this reset is owed on teardown rather than only on rebind.
    runOptionalWinRt("clearing the SMTC thumbnail",
                     [this]
                     {
                       auto updater = _statePtr->controls.DisplayUpdater();
                       updater.Thumbnail(nullptr);
                       updater.Update();
                     });
    runOptionalWinRt("disabling SMTC controls", [this] { _statePtr->controls.IsEnabled(false); });
  }

  void SmtcBridge::handleSnapshot(rt::PlaybackSnapshot const& snapshot)
  {
    if (!_statePtr->active)
    {
      return;
    }

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
      _artworkRequest.reset();
    }
    else
    {
      updateArtwork(transport.nowPlaying.coverArtId);
    }
  }

  void SmtcBridge::updateArtwork(ResourceId const resourceId)
  {
    if (!_statePtr->active || _resourceBytes == nullptr || _runtime == nullptr)
    {
      return;
    }

    _artworkRequest.reset();
    _artworkTask.reset();
    auto const statePtr = std::weak_ptr<State>{_statePtr};
    _artworkRequest = _resourceBytes->request(
      resourceId,
      [this, statePtr, resourceId](rt::ResourceBytes bytes)
      {
        if (auto lockedStatePtr = statePtr.lock(); lockedStatePtr && lockedStatePtr->active &&
                                                   lockedStatePtr->displayedArtworkId == resourceId && !bytes.empty() &&
                                                   _runtime != nullptr)
        {
          auto* const runtime = &_runtime->async();
          _artworkTask = runtime->spawnCancellable(
            [statePtr, runtime, resourceId, bytes = std::move(bytes)](std::stop_token const stopToken) mutable
            { return prepareAndWriteArtwork(statePtr, runtime, resourceId, std::move(bytes), stopToken); },
            "Windows SMTC cover-art stream preparation");
        }
      });
  }

  async::Task<void> SmtcBridge::prepareAndWriteArtwork(std::weak_ptr<State> statePtr,
                                                       async::Runtime* const runtime,
                                                       ResourceId const resourceId,
                                                       rt::ResourceBytes bytes,
                                                       std::stop_token const stopToken)
  {
    auto prepared = PreparedMemoryRandomAccessStream{};

    co_await runtime->resumeOnWorker(stopToken);
    prepared = prepareMemoryRandomAccessStream(bytes.view());

    co_await runtime->resumeOnCallbackExecutor(stopToken);

    if (auto lockedStatePtr = statePtr.lock();
        lockedStatePtr && lockedStatePtr->active && lockedStatePtr->displayedArtworkId == resourceId)
    {
      writeArtworkStream(*lockedStatePtr, resourceId, std::move(prepared));
    }
  }

  void SmtcBridge::writeArtworkStream(State& state,
                                      ResourceId const resourceId,
                                      PreparedMemoryRandomAccessStream prepared)
  {
    if (!prepared)
    {
      return;
    }

    try
    {
      auto stream = makeMemoryRandomAccessStream(std::move(prepared));

      if (!state.active || state.displayedArtworkId != resourceId)
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

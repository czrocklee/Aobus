// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlaybackControls.h"

#include "playback/OutputDeviceControl.h"
#include "playback/PlaybackTimeControl.h"
#include "playback/SeekControl.h"
#include "playback/SoulTransportButton.h"
#include "playback/TransportButton.h"
#include "playback/VolumeControl.h"
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <memory>
#include <utility>
#include <vector>

namespace ao::winui
{
  namespace
  {
    std::unique_ptr<TransportButton> makeTransportButton(winrt::Microsoft::UI::Xaml::Controls::Button button,
                                                         uimodel::PlaybackCommand const command)
    {
      return std::make_unique<TransportButton>(TransportButtonConfig{
        .button = std::move(button),
        .command = command,
      });
    }
  } // namespace

  struct PlaybackControls::Impl final
  {
    explicit Impl(PlaybackControlsConfig config)
    {
      transportButtons.push_back(
        makeTransportButton(std::move(config.modern.shuffleButton), uimodel::PlaybackCommand::ToggleShuffle));
      transportButtons.push_back(
        makeTransportButton(std::move(config.modern.previousButton), uimodel::PlaybackCommand::Previous));
      transportButtons.push_back(
        makeTransportButton(std::move(config.classic.previousButton), uimodel::PlaybackCommand::Previous));
      transportButtons.push_back(
        makeTransportButton(std::move(config.modern.nextButton), uimodel::PlaybackCommand::Next));
      transportButtons.push_back(
        makeTransportButton(std::move(config.classic.nextButton), uimodel::PlaybackCommand::Next));
      transportButtons.push_back(
        makeTransportButton(std::move(config.modern.repeatButton), uimodel::PlaybackCommand::CycleRepeat));
      transportButtons.push_back(
        makeTransportButton(std::move(config.classic.playPauseButton), uimodel::PlaybackCommand::PlayPause));
      auto stopButtonPtr = makeTransportButton(std::move(config.classic.stopButton), uimodel::PlaybackCommand::Stop);
      stopButton = stopButtonPtr.get();
      transportButtons.push_back(std::move(stopButtonPtr));

      soulTransport = std::make_unique<SoulTransportButton>(SoulTransportButtonConfig{
        .button = std::move(config.modern.soulButton),
        .soul = std::move(config.modern.soul),
        .hasComplexTooltip = true,
      });
      outputDevice = std::make_unique<OutputDeviceControl>(OutputDeviceControlConfig{
        .modernButton = std::move(config.modern.outputButton),
        .classicButton = std::move(config.classic.soulButton),
      });

      seekControls.push_back(std::make_unique<SeekControl>(SeekControlConfig{
        .slider = std::move(config.modern.seek),
        .presentationActive = true,
        .modernOverlay = true,
      }));
      seekControls.push_back(std::make_unique<SeekControl>(SeekControlConfig{
        .slider = std::move(config.classic.seek),
        .presentationActive = false,
        .modernOverlay = false,
      }));

      timeControls.push_back(std::make_unique<PlaybackTimeControl>(PlaybackTimeControlConfig{
        .text = std::move(config.modern.elapsed),
        .mode = uimodel::PlaybackTimeMode::Elapsed,
        .presentationActive = true,
      }));
      timeControls.push_back(std::make_unique<PlaybackTimeControl>(PlaybackTimeControlConfig{
        .text = std::move(config.modern.duration),
        .mode = uimodel::PlaybackTimeMode::Duration,
        .presentationActive = true,
      }));
      timeControls.push_back(std::make_unique<PlaybackTimeControl>(PlaybackTimeControlConfig{
        .text = std::move(config.classic.time),
        .mode = uimodel::PlaybackTimeMode::Default,
        .presentationActive = false,
      }));

      volumeControls.push_back(
        std::make_unique<VolumeControl>(VolumeControlConfig{.slider = std::move(config.modern.volume)}));
      volumeControls.push_back(
        std::make_unique<VolumeControl>(VolumeControlConfig{.slider = std::move(config.classic.volume)}));
    }

    void bind(WinUiDependencies const& dependencies)
    {
      unbind();
      for (auto const& buttonPtr : transportButtons)
      {
        buttonPtr->bind(dependencies);
      }
      soulTransport->bind(dependencies);
      outputDevice->bind(dependencies);
      for (auto const& controlPtr : seekControls)
      {
        controlPtr->bind(dependencies);
      }
      for (auto const& controlPtr : timeControls)
      {
        controlPtr->bind(dependencies);
      }
      for (auto const& controlPtr : volumeControls)
      {
        controlPtr->bind(dependencies);
      }
    }

    void unbind()
    {
      for (auto const& buttonPtr : transportButtons)
      {
        buttonPtr->unbind();
      }
      soulTransport->unbind();
      outputDevice->unbind();
      for (auto const& controlPtr : seekControls)
      {
        controlPtr->unbind();
      }
      for (auto const& controlPtr : timeControls)
      {
        controlPtr->unbind();
      }
      for (auto const& controlPtr : volumeControls)
      {
        controlPtr->unbind();
      }
    }

    void setPresentationActive(bool const modern)
    {
      seekControls[0]->setPresentationActive(modern);
      seekControls[1]->setPresentationActive(!modern);
      timeControls[0]->setPresentationActive(modern);
      timeControls[1]->setPresentationActive(modern);
      timeControls[2]->setPresentationActive(!modern);
    }

    std::vector<std::unique_ptr<TransportButton>> transportButtons;
    TransportButton* stopButton = nullptr;
    std::unique_ptr<SoulTransportButton> soulTransport;
    std::unique_ptr<OutputDeviceControl> outputDevice;
    std::vector<std::unique_ptr<SeekControl>> seekControls;
    std::vector<std::unique_ptr<PlaybackTimeControl>> timeControls;
    std::vector<std::unique_ptr<VolumeControl>> volumeControls;
  };

  PlaybackControls::PlaybackControls(PlaybackControlsConfig config)
    : _implPtr{std::make_unique<Impl>(std::move(config))}
  {
  }

  PlaybackControls::~PlaybackControls()
  {
    unbind();
  }

  void PlaybackControls::bind(WinUiDependencies const& dependencies)
  {
    _implPtr->bind(dependencies);
  }

  void PlaybackControls::unbind()
  {
    _implPtr->unbind();
  }

  void PlaybackControls::setPresentationActive(bool const modern)
  {
    _implPtr->setPresentationActive(modern);
  }

  void PlaybackControls::activatePlayPause()
  {
    _implPtr->soulTransport->activate();
  }

  void PlaybackControls::activateStop()
  {
    _implPtr->stopButton->activate();
  }
} // namespace ao::winui

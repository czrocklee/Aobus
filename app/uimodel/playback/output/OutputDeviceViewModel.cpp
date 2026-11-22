// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/audio/PipeWireTargetIdentity.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/PlaybackOutputText.h>

#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    audio::OutputDeviceSelection const& requestedOutputDevice(rt::OutputState const& output)
    {
      return output.optPendingOutputSelection ? *output.optPendingOutputSelection : output.selectedDevice;
    }

    std::string pendingOutputDeviceStatus(i18n::MessageCatalog const& catalog,
                                          rt::OutputState const& output,
                                          audio::OutputDeviceSelection const& pending)
    {
      if (pending.backendId == audio::kBackendPipeWire && !pending.deviceId.empty() &&
          !audio::pipeWireTargetNodeName(pending.deviceId.raw()))
      {
        return std::string{i18n::requiredText(catalog, i18n::MessageId::OutputDeviceInvalidPipeWireStatus)};
      }

      auto name = outputDeviceName(catalog, output, pending).value_or(std::string{});

      if (name.empty() && pending.backendId != audio::kBackendNone && pending.deviceId.empty() &&
          pending.profileId == audio::kProfileShared)
      {
        name = i18n::requiredText(catalog, i18n::MessageId::SystemDefaultOutputDevice);
      }

      if (name.empty())
      {
        return std::string{i18n::requiredText(catalog, i18n::MessageId::OutputDevicePendingStatus)};
      }

      return i18n::requiredFormat(catalog, i18n::MessageId::OutputDevicePendingNamedStatus, {{"device", name}});
    }

    void projectOutputStatus(OutputDeviceViewState& view,
                             i18n::MessageCatalog const& catalog,
                             rt::OutputState const& output)
    {
      if (output.optPendingOutputSelection)
      {
        view.outputBackendSummary = i18n::requiredText(catalog, i18n::MessageId::OutputDevicePendingSummary);
        view.outputDeviceStatus = pendingOutputDeviceStatus(catalog, output, *output.optPendingOutputSelection);
        view.rows.push_back(OutputDeviceRow{.kind = OutputDeviceRow::Kind::Status, .title = view.outputDeviceStatus});
        return;
      }

      auto const& selected = output.selectedDevice;

      if (selected.backendId == audio::kBackendNone)
      {
        view.outputBackendSummary = "--";
        return;
      }

      view.hasActiveOutputDevice = true;
      auto const backend = audioBackendPresentation(catalog, selected.backendId);
      view.outputBackendSummary = backend.shortLabel;

      auto const optName = outputDeviceName(catalog, output, selected);
      bool const hasName = optName && !optName->empty();
      bool const allowReroute = selected.routingPolicy == audio::OutputRoutingPolicy::AllowSystemReroute;

      if (allowReroute && !hasName)
      {
        view.outputDeviceStatus = i18n::requiredText(catalog, i18n::MessageId::OutputDeviceRerouteStatus);
        return;
      }

      auto const& deviceName = hasName ? *optName : selected.deviceId.raw();
      auto route = std::format("{}: {}", backend.label, deviceName);

      if (selected.profileId == audio::kProfileExclusive)
      {
        route += " (" + audioProfilePresentation(catalog, audio::kProfileExclusive).label + ")";
      }

      view.outputDeviceStatus =
        allowReroute ? i18n::requiredFormat(catalog, i18n::MessageId::OutputDevicePreferredStatus, {{"route", route}})
                     : std::move(route);
    }

    void appendRoutingPolicyRow(std::vector<OutputDeviceRow>& rows,
                                i18n::MessageCatalog const& catalog,
                                rt::OutputState const& output)
    {
      if (auto const& requested = requestedOutputDevice(output); audio::supportsOutputRoutingPolicy(requested))
      {
        bool const allowReroute = requested.routingPolicy == audio::OutputRoutingPolicy::AllowSystemReroute;
        auto const sharedDescriptionId = allowReroute ? i18n::MessageId::OutputDeviceRoutingPolicyAllowed
                                                      : i18n::MessageId::OutputDeviceRoutingPolicyFixed;
        auto description = requested.profileId == audio::kProfileExclusive
                             ? i18n::requiredFormat(catalog,
                                                    i18n::MessageId::OutputDeviceRoutingPolicyExclusive,
                                                    {{"rerouting", allowReroute ? "allowed" : "fixed"}})
                             : std::string{i18n::requiredText(catalog, sharedDescriptionId)};
        rows.push_back(OutputDeviceRow{
          .kind = OutputDeviceRow::Kind::RoutingPolicy,
          .title = std::string{i18n::requiredText(catalog, i18n::MessageId::OutputDeviceRoutingPolicyToggle)},
          .description = std::move(description),
          .isActive = allowReroute,
          .optRoutingPolicySelection = requested,
        });
      }
    }
  } // namespace

  OutputDeviceViewModel::OutputDeviceViewModel(rt::PlaybackService& playback,
                                               i18n::MessageCatalog textCatalog,
                                               RenderCallback onRender,
                                               OutputDeviceIntent intent)
    : _playback{playback}
    , _commands{playback.commands()}
    , _onRender{std::move(onRender)}
    , _intent{std::move(intent)}
    , _textCatalog{std::move(textCatalog)}
    , _lastOutput{playback.snapshot().transport.output}
  {
    _snapshotSub =
      _playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) { handleSnapshot(snapshot); });
  }

  void OutputDeviceViewModel::selectOutputDevice(audio::BackendId const& backendId,
                                                 audio::DeviceId const& deviceId,
                                                 audio::ProfileId const& profileId)
  {
    auto selection = audio::OutputDeviceSelection{
      .backendId = backendId,
      .deviceId = deviceId,
      .profileId = profileId,
    };
    auto const& requested = requestedOutputDevice(_playback.snapshot().transport.output);

    if (selection.backendId == requested.backendId && selection.deviceId == requested.deviceId &&
        audio::supportsOutputRoutingPolicy(selection))
    {
      selection.routingPolicy = requested.routingPolicy;
    }

    submitOutputDevice(std::move(selection));
  }

  bool OutputDeviceViewModel::tryToggleOutputRoutingPolicy(audio::OutputDeviceSelection const& expected)
  {
    auto selection = requestedOutputDevice(_playback.snapshot().transport.output);

    if (selection != expected || !audio::supportsOutputRoutingPolicy(selection))
    {
      return false;
    }

    selection.routingPolicy = selection.routingPolicy == audio::OutputRoutingPolicy::Fixed
                                ? audio::OutputRoutingPolicy::AllowSystemReroute
                                : audio::OutputRoutingPolicy::Fixed;
    submitOutputDevice(std::move(selection));
    return true;
  }

  void OutputDeviceViewModel::submitOutputDevice(audio::OutputDeviceSelection selection)
  {
    _commands.setOutputDevice(selection);
    _intent.record(selection);
  }

  void OutputDeviceViewModel::refresh()
  {
    auto const& output = _playback.snapshot().transport.output;
    _lastOutput = output;
    render(output);
  }

  void OutputDeviceViewModel::stopObserving()
  {
    _snapshotSub.reset();
  }

  void OutputDeviceViewModel::handleSnapshot(rt::PlaybackSnapshot const& snapshot)
  {
    if (snapshot.transport.output == _lastOutput)
    {
      return;
    }

    _lastOutput = snapshot.transport.output;
    render(snapshot.transport.output);
  }

  void OutputDeviceViewModel::render(rt::OutputState const& output)
  {
    auto view = OutputDeviceViewState{};
    projectOutputStatus(view, _textCatalog, output);
    appendRoutingPolicyRow(view.rows, _textCatalog, output);
    auto const& selected = output.selectedDevice;

    for (auto const& backend : output.availableBackends)
    {
      auto const backendPresentation = audioBackendPresentation(_textCatalog, backend.id);
      view.rows.push_back(OutputDeviceRow{
        .kind = OutputDeviceRow::Kind::BackendHeader,
        .backendId = backend.id,
        .deviceId = audio::DeviceId{},
        .profileId = audio::kProfileShared,
        .title = backendPresentation.label,
        .description = backendPresentation.description,
        .isActive = false,
      });

      for (auto const& device : backend.devices)
      {
        for (auto const& supportedProfile : backend.supportedProfiles)
        {
          auto const profile = supportedProfile.id;

          if (!rt::supportsOutputProfile(device, profile))
          {
            continue;
          }

          bool const isExclusive = (profile == audio::kProfileExclusive);
          auto deviceTitle = device.displayName;

          if (device.isDefault && device.id.empty() && deviceTitle.empty())
          {
            deviceTitle = i18n::requiredText(_textCatalog, i18n::MessageId::SystemDefaultOutputDevice);
          }

          bool const isActive = view.hasActiveOutputDevice && backend.id == selected.backendId &&
                                profile == selected.profileId && device.id == selected.deviceId;

          view.rows.push_back(OutputDeviceRow{
            .kind = OutputDeviceRow::Kind::DeviceProfile,
            .backendId = backend.id,
            .deviceId = device.id,
            .profileId = profile,
            .title = std::move(deviceTitle),
            .description =
              device.description.empty() ? backendPresentation.outputDeviceDescriptionFallback : device.description,
            .isActive = isActive,
            .isExclusive = isExclusive,
          });
        }
      }
    }

    if (_onRender)
    {
      _onRender(view);
    }
  }

  void OutputDeviceViewModel::setTextCatalog(i18n::MessageCatalog textCatalog)
  {
    _textCatalog = std::move(textCatalog);
    refresh();
  }
} // namespace ao::uimodel

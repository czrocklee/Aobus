// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/ShellBuilder.h"

#include "app/LibrarySession.h"
#include "input/KeymapAccelerators.h"
#include "input/SystemCharacterKey.h"
#include "layout/ShellPresetSource.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/FocusedDetail.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutHost.h"
#include "layout/runtime/ShellLibraryAccess.h"
#include "pch.h"
#include "platform/StringResources.h"
#include "theme/SurfaceBrushes.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/layout/shell/LayoutBuildStateView.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/input/KeymapAcceleratorPlan.h>
#include <ao/winui/layout/LayoutCatalog.h>
#include <ao/winui/layout/ShellDocument.h>
#include <ao/winui/layout/ShellStatePolicy.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::TextWrapping;
    using winrt::Microsoft::UI::Xaml::Thickness;
    using winrt::Microsoft::UI::Xaml::Controls::MenuBar;
    using winrt::Microsoft::UI::Xaml::Controls::MenuBarItem;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyout;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;

    constexpr auto kModernOverflowMenuId = std::string_view{"modernOverflow"};
    constexpr auto kNowPlayingOverflowMenuId = std::string_view{"nowPlayingOverflow"};
    constexpr double kFatalLayoutErrorPadding = 24.0;

    ShellPreset presetForMode(ShellMode const mode) noexcept
    {
      return mode == ShellMode::Classic ? ShellPreset::Classic : ShellPreset::Modern;
    }

    ShellLibraryAccess makeLibraryAccess(LibrarySession& sessionValue, ShellListCommands const& listCommands)
    {
      auto* const session = &sessionValue;
      return ShellLibraryAccess{
        .libraryRoot = session->runtime().musicRoot(),
        .listTreeProjection =
          [session]
        {
          return uimodel::buildListTreeProjection(
            session->textCatalog(), session->runtime().library().snapshot().lists());
        },
        .subscribeListTreeChanged =
          [session](compat::MoveOnlyFunction<void()> handler)
        {
          return session->runtime().library().changes().onChanged(
            [handler = std::move(handler)](rt::LibraryChangeSet const& changeSet) mutable
            {
              if (listTreeChangeRequiresRebuild(changeSet))
              {
                handler();
              }
            });
        },
        .preferredPresentation = [session](ListId const listId) -> std::optional<rt::TrackPresentationSpec>
        {
          if (!session->listPresentations().presentationIdForList(listId))
          {
            return std::nullopt;
          }

          return session->presentationForList(listId);
        },
        .playTrack = [session](rt::ViewId const viewId, TrackId const trackId)
        { return session->playTrack(viewId, trackId); },
        .createList = listCommands.createList,
        .editList = listCommands.editList,
        .deleteList = listCommands.deleteList,
        .membershipTargets = listCommands.membershipTargets,
        .editMembership = listCommands.editMembership,
        .orderCapabilities = listCommands.orderCapabilities,
        .applyOrder = listCommands.applyOrder,
      };
    }

    /// A menu item that runs @p command, or nothing at all when the frame offers none.
    void appendItem(winrt::Windows::Foundation::Collections::IVector<
                      winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase> const& items,
                    std::string_view const resourceId,
                    std::function<void()> const& command,
                    std::string_view const acceleratorText = {})
    {
      if (!command)
      {
        return;
      }

      auto item = MenuFlyoutItem{};
      item.Text(winrt::to_hstring(resourceString(resourceId)));

      if (!acceleratorText.empty())
      {
        item.KeyboardAcceleratorTextOverride(winrt::to_hstring(acceleratorText));
      }

      item.Click([command](winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) { command(); });
      items.Append(item);
    }

    void appendSeparator(
      winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase> const&
        items)
    {
      // A separator that opens or closes a menu separates nothing, which happens
      // whenever the frame withholds the commands on one side of it.
      if (items.Size() == 0)
      {
        return;
      }

      items.Append(MenuFlyoutSeparator{});
    }
  } // namespace

  ShellBuilder::ShellBuilder(LibrarySession& session, ShellBuilderConfig config)
    : _session{session}
    , _config{std::move(config)}
    , _libraryAccess{makeLibraryAccess(session, _config.listCommands)}
    , _host{_config.host}
  {
    registerActions();
    installKeyboardAccelerators();
  }

  ShellBuilder::~ShellBuilder()
  {
    retire();
  }

  void ShellBuilder::registerActions()
  {
    auto const bindCommand = [this](std::string_view const id, std::function<void()> const& command)
    {
      if (!command)
      {
        return;
      }

      _actions.registerAction(id, [command](ActionContext const&) { command(); });
    };

    // The transport is the one action family a keyboard map binds by default,
    // and the surface that runs it outlives every generation, so it is bound
    // here rather than left to the buttons that also invoke it.
    auto& playback = _session.playbackActions();

    for (auto const command : uimodel::playbackCommands())
    {
      _actions.registerAction(uimodel::playbackCommandActionId(command),
                              [&playback, command](ActionContext const&) { playback.execute(command); });
    }

    bindCommand("library.open", _config.commands.openLibrary);
    bindCommand("library.rescan", _config.commands.rescanLibrary);
    bindCommand("shell.toggleInspector", _config.commands.toggleInspector);
    bindCommand("shell.showSoul", _config.commands.showSoul);
    bindCommand("shell.showSystemMenu", _config.commands.showSystemMenu);
    bindCommand("workspace.revealCurrentTrack", _config.commands.revealCurrentTrack);
    bindCommand("track.presentProperties", _config.commands.presentTrackProperties);

    if (auto const& applyOrder = _config.listCommands.applyOrder; applyOrder)
    {
      _actions.registerAction(
        "track.orderMoveUp", [applyOrder](ActionContext const&) { applyOrder(ListOrderCommand::MoveUp); });
      _actions.registerAction(
        "track.orderMoveDown", [applyOrder](ActionContext const&) { applyOrder(ListOrderCommand::MoveDown); });
      _actions.registerAction(
        "track.orderMoveToTop", [applyOrder](ActionContext const&) { applyOrder(ListOrderCommand::MoveToTop); });
      _actions.registerAction(
        "track.orderMoveToBottom", [applyOrder](ActionContext const&) { applyOrder(ListOrderCommand::MoveToBottom); });
    }

    // The selector presents from wherever it was raised, so unlike the rest this
    // one is the anchor's business as much as the shell's.
    if (auto const& showSelector = _config.commands.showOutputDeviceSelector; showSelector)
    {
      _actions.registerAction("playback.showOutputDeviceSelector",
                              [showSelector](ActionContext const& context) { showSelector(context.anchor); });
    }
  }

  PaneSettingsAccess ShellBuilder::paneSettings()
  {
    return {
      .navigationWidth = [this] { return _session.settings().navigationPaneWidth; },
      .setNavigationWidth = [this](double const width) { _session.settings().navigationPaneWidth = width; },
      .inspectorWidth = [this] { return _session.settings().inspectorPaneWidth; },
      .setInspectorWidth = [this](double const width) { _session.settings().inspectorPaneWidth = width; },
      .commit =
        [this]
      {
        if (_config.saveSettings)
        {
          _config.saveSettings();
        }
      },
    };
  }

  MenuFlyout ShellBuilder::modernOverflowFlyout() const
  {
    auto flyout = MenuFlyout{};
    auto const items = flyout.Items();
    appendItem(items, "winui_shell_open_library", _config.commands.openLibrary);
    appendItem(items, "winui_shell_rescan_library", _config.commands.rescanLibrary);
    appendItem(items, "winui_shell_import_library_data", _config.commands.importLibrary);
    appendItem(items, "winui_shell_export_library_data", _config.commands.exportLibrary);
    appendSeparator(items);
    appendItem(items, "winui_track_properties_command", _config.commands.presentTrackProperties, "Alt+Enter");
    appendItem(items, "winui_shell_columns", _config.commands.chooseColumns);
    appendItem(items, "winui_shell_classic_mode", _config.commands.toggleShellMode);
    appendItem(items, "winui_shell_reload_theme", _config.commands.reloadTheme);
    return flyout;
  }

  MenuFlyout ShellBuilder::nowPlayingOverflowFlyout() const
  {
    auto flyout = MenuFlyout{};
    auto const items = flyout.Items();
    appendItem(items, "winui_shell_stop", _config.commands.stop);
    appendItem(items, "winui_shell_reveal_current_track", _config.commands.revealCurrentTrack);
    appendItem(items, "winui_shell_open_library", _config.commands.openLibrary);
    appendItem(items, "winui_shell_rescan_library", _config.commands.rescanLibrary);
    appendItem(items, "winui_shell_import_library_data", _config.commands.importLibrary);
    appendItem(items, "winui_shell_export_library_data", _config.commands.exportLibrary);
    appendSeparator(items);
    appendItem(items, "winui_shell_classic_mode", _config.commands.toggleShellMode);
    return flyout;
  }

  void ShellBuilder::composeMenuBar(MenuBar const& bar) const
  {
    auto const appendMenu = [&bar](std::string_view const titleResourceId, auto const& fill)
    {
      auto menu = MenuBarItem{};
      menu.Title(winrt::to_hstring(resourceString(titleResourceId)));
      fill(menu.Items());

      // A titled menu that opens on nothing is worse than no menu at all.
      if (menu.Items().Size() > 0)
      {
        bar.Items().Append(menu);
      }
    };

    appendMenu("winui_shell_menu_file",
               [this](auto const& items)
               {
                 appendItem(items, "winui_shell_open_library", _config.commands.openLibrary);
                 appendItem(items, "winui_shell_rescan", _config.commands.rescanLibrary);
                 appendSeparator(items);
                 appendItem(items, "winui_shell_import_library_data", _config.commands.importLibrary);
                 appendItem(items, "winui_shell_export_library_data", _config.commands.exportLibrary);
               });
    appendMenu("winui_shell_menu_view",
               [this](auto const& items)
               {
                 appendItem(items, "winui_shell_columns", _config.commands.chooseColumns);
                 // Classic authors no inspector toggle of its own, so the menu
                 // is the only place the overlay can be asked for at the widths
                 // that do not seat the inspector inline.
                 appendItem(items, "winui_shell_track_details", _config.commands.toggleInspector);
                 appendItem(
                   items, "winui_track_properties_command", _config.commands.presentTrackProperties, "Alt+Enter");
                 appendItem(items, "winui_shell_modern_mode", _config.commands.toggleShellMode);
                 appendItem(items, "winui_shell_reload_theme", _config.commands.reloadTheme);
               });
    appendMenu("winui_shell_menu_playback",
               [this](auto const& items)
               {
                 appendItem(items, "winui_shell_play_pause", _config.commands.playPause);
                 appendItem(items, "winui_shell_stop", _config.commands.stop);
                 appendItem(items, "winui_shell_reveal_current_track", _config.commands.revealCurrentTrack);
               });
  }

  MenuComposer ShellBuilder::menus()
  {
    return {
      .flyout = [this](std::string_view const menuId) -> MenuFlyout
      {
        if (menuId == kModernOverflowMenuId)
        {
          return modernOverflowFlyout();
        }

        if (menuId == kNowPlayingOverflowMenuId)
        {
          return nowPlayingOverflowFlyout();
        }

        return MenuFlyout{nullptr};
      },
      .composeMenuBar = [this](MenuBar const& bar) { composeMenuBar(bar); },
    };
  }

  Result<> ShellBuilder::build(ShellPreset const preset)
  {
    auto preparedRes = prepareShellPreset(preset);

    if (!preparedRes)
    {
      return std::unexpected{preparedRes.error()};
    }

    // The preset id keys the component runtime state, so it names the candidate
    // being built rather than the generation that is still live.
    auto const previousPresetId = std::exchange(_runtimeState.activePresetId, std::string{shellPresetId(preset)});
    auto generation = ShellGeneration{};

    try
    {
      generation.gatePtr = _host.stage();
      generation.focusedDetailPtr = std::make_shared<FocusedDetail>();
      auto& runtime = _session.runtime();
      auto context = LayoutBuildContext{
        .asyncRuntime = runtime.async(),
        .playback = runtime.playback(),
        .views = runtime.views(),
        .workspace = runtime.workspace(),
        .notifications = runtime.notifications(),
        .libraryJobs = runtime.library().jobs(),
        .completion = runtime.completion(),
        .playbackActions = _session.playbackActions(),
        .presentationCatalog = _session.presentationCatalog(),
        .listPresentations = _session.listPresentations(),
        .textCatalog = _session.textCatalog(),
        .trackList = _config.trackList,
        .resourceBytes = _config.resourceBytes,
        .theme = _config.theme,
        .library = _libraryAccess,
        .catalog = layoutCatalog(),
        .actions = _actions,
        .resources = _config.resources,
        .surfaceBrush = makeSurfaceBrushResolver(_config.activeTheme ? _config.activeTheme() : std::optional<Theme>{}),
        .runtimeState = _runtimeState,
        .buildState = uimodel::LayoutBuildStateView{_runtimeState},
        .shellState = _shellState,
        .shellStateChanged = _shellStateChanged,
        .windowActivity = _windowActivity,
        .windowActivityChanged = _windowActivityChanged,
        .statusMessage = _statusMessage,
        .statusMessageChanged = _statusMessageChanged,
        .gatePtr = generation.gatePtr,
        .paneSettings = paneSettings(),
        .outputDeviceIntent = uimodel::OutputDeviceIntent::recordedBy(
          [this](audio::OutputDeviceSelection const& selection) { _session.setPreferredOutputSelection(selection); }),
        .menus = menus(),
        .reportStatus = [this](std::string message) { reportStatus(std::move(message)); },
        .focusedDetailPtr = generation.focusedDetailPtr,
        .titleBarSlot = generation.titleBarElement,
      };

      auto builtRes = _registry.build(context, preparedRes->effectiveRoot());

      if (!builtRes)
      {
        _host.discard(std::move(generation));
        _runtimeState.activePresetId = previousPresetId;
        return std::unexpected{builtRes.error()};
      }

      generation.rootPtr = std::move(builtRes->componentPtr);

      if (auto publishedRes = _host.publish(std::move(generation)); !publishedRes)
      {
        _runtimeState.activePresetId = previousPresetId;
        return std::unexpected{publishedRes.error()};
      }

      _optLivePreset = preset;
      _optRejectedPreset.reset();
      return {};
    }
    catch (...)
    {
      _host.discard(std::move(generation));
      _runtimeState.activePresetId = previousPresetId;
      throw;
    }
  }

  Result<ShellState> ShellBuilder::applyShellState(ShellMode const mode,
                                                   double const width,
                                                   std::optional<bool> optInspectorRequest)
  {
    if (_retired)
    {
      return _shellState;
    }

    auto const targetState = ShellStatePolicy::resolve(mode, width, optInspectorRequest);

    if (auto const preset = presetForMode(targetState.mode); _optLivePreset != preset)
    {
      if (_optRejectedPreset == preset)
      {
        return _shellState;
      }

      if (auto builtRes = build(preset); !builtRes)
      {
        _optRejectedPreset = preset;

        if (!_host.hasActiveGeneration())
        {
          showFatalLayoutError(builtRes.error());
        }

        return std::unexpected{builtRes.error()};
      }
    }

    // State publication comes after any required build. A rejected candidate
    // therefore cannot change what the generation that stayed live observes.
    if (targetState != _shellState)
    {
      _shellState = targetState;
      _shellStateChanged.emit(_shellState);
    }

    return _shellState;
  }

  Result<> ShellBuilder::rebuild()
  {
    if (_retired || !_optLivePreset)
    {
      return {};
    }

    // An explicit re-attempt: whatever was rejected before is worth trying
    // again once the frame asks for a rebuild.
    _optRejectedPreset.reset();
    return build(*_optLivePreset);
  }

  void ShellBuilder::applyWindowActivity(bool const visible, bool const minimized)
  {
    auto const activity = WindowActivityState{.visible = visible, .minimized = minimized};

    if (_retired || activity == _windowActivity)
    {
      return;
    }

    _windowActivity = activity;
    _windowActivityChanged.emit(_windowActivity);
  }

  void ShellBuilder::reportStatus(std::string message)
  {
    if (_retired || message == _statusMessage)
    {
      return;
    }

    _statusMessage = std::move(message);
    _statusMessageChanged.emit(_statusMessage);
  }

  void ShellBuilder::showFatalLayoutError(Error const& error)
  {
    APP_LOG_CRITICAL("ShellBuilder: no shell could be built: {}", error.message);

    if (!_config.host)
    {
      return;
    }

    auto text = TextBlock{};
    text.Text(winrt::to_hstring(formatResource("winui_shell_layout_failed", error.message)));
    text.TextWrapping(TextWrapping::Wrap);
    text.Margin(Thickness{.Left = kFatalLayoutErrorPadding,
                          .Top = kFatalLayoutErrorPadding,
                          .Right = kFatalLayoutErrorPadding,
                          .Bottom = kFatalLayoutErrorPadding});

    try
    {
      _config.host.Child(text);
    }
    catch (winrt::hresult_error const& attachFailure)
    {
      APP_LOG_CRITICAL(
        "ShellBuilder: the layout error surface could not be shown: {}", winrt::to_string(attachFailure.message()));
    }
  }

  void ShellBuilder::installKeyboardAccelerators()
  {
    // The frame's host region outlives every generation, so a shortcut keeps
    // working across a shell switch instead of being rebuilt with the tree.
    // Asking the system where each character sits is what makes a punctuation
    // shortcut land on the key the user pressed rather than the one a US
    // keyboard would have carried.
    auto const plans = planKeymapAccelerators(
      _session.keymap(),
      layoutActionCatalog(),
      [this](std::string_view const id) { return _actions.contains(id); },
      systemCharacterKeyResolver());

    // The handler outlives this builder if construction throws after the
    // accelerators are on the host, so it proves the builder is still there
    // before reaching into it.
    applyKeymapAccelerators(_config.host,
                            plans,
                            [this, lifetimePtr = std::weak_ptr{_lifetimePtr}](std::string_view const id)
                            { return !lifetimePtr.expired() && _actions.invoke(id, ActionContext{}); });
  }

  bool ShellBuilder::invokeAction(std::string_view const actionId) const
  {
    return !_retired && _actions.invoke(actionId, ActionContext{});
  }

  void ShellBuilder::retire() noexcept
  {
    if (_retired)
    {
      return;
    }

    // Close generation admission before detaching the native root. Components
    // can therefore observe retirement even if XAML refuses the detach.
    _retired = true;
    // The accelerators hold a callable into this builder, so they go first.
    // Clearing cannot throw out of here; the weak lifetime token is what makes
    // a handler that survives a failed clear inert rather than dangerous.
    clearKeymapAccelerators(_config.host);
    _host.retire();
    _optLivePreset.reset();
    _optRejectedPreset.reset();
  }
} // namespace ao::winui::layout

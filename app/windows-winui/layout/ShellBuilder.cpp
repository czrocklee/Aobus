// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/ShellBuilder.h"

#include "app/LibrarySession.h"
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
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/uimodel/layout/shell/LayoutBuildStateView.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/layout/LayoutCatalog.h>
#include <ao/winui/layout/ShellDocument.h>
#include <ao/winui/layout/ShellStatePolicy.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
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

    ShellLibraryAccess makeLibraryAccess(LibrarySession& sessionValue)
    {
      auto* const session = &sessionValue;
      return ShellLibraryAccess{
        .libraryRoot = session->runtime().musicRoot(),
        .listTreeProjection = [session]
        { return uimodel::buildListTreeProjection(session->runtime().library().reader().lists()); },
        .preferredPresentation = [session](ListId const listId) -> std::optional<rt::TrackPresentationSpec>
        {
          if (!session->presentationPreferences().presentations.contains(listId))
          {
            return std::nullopt;
          }

          return session->presentationForList(listId);
        },
        .rememberPresentation =
          [session](ListId const listId, std::string presentationId)
        {
          session->presentationPreferences().presentations[listId] = std::move(presentationId);
          std::ignore = session->saveSettings();
        },
        .playTrack = [session](rt::ViewId const viewId, TrackId const trackId)
        { return session->playTrack(viewId, trackId); },
      };
    }

    /// A menu item that runs @p command, or nothing at all when the frame offers none.
    void appendItem(winrt::Windows::Foundation::Collections::IVector<
                      winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase> const& items,
                    std::string_view const resourceId,
                    std::function<void()> const& command)
    {
      if (!command)
      {
        return;
      }

      auto item = MenuFlyoutItem{};
      item.Text(winrt::to_hstring(resourceString(resourceId)));
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
    : _session{session}, _config{std::move(config)}, _libraryAccess{makeLibraryAccess(session)}, _host{_config.host}
  {
    registerActions();
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

    bindCommand("library.open", _config.commands.openLibrary);
    bindCommand("library.rescan", _config.commands.rescanLibrary);
    bindCommand("shell.toggleInspector", _config.commands.toggleInspector);
    bindCommand("shell.showSoul", _config.commands.showSoul);
    bindCommand("shell.showSystemMenu", _config.commands.showSystemMenu);

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
    appendItem(items, "ModernOpenLibraryMenuItem", _config.commands.openLibrary);
    appendItem(items, "ModernRescanMenuItem", _config.commands.rescanLibrary);
    appendSeparator(items);
    appendItem(items, "ModernColumnsMenuItem", _config.commands.chooseColumns);
    appendItem(items, "ClassicModeMenuItem", _config.commands.toggleShellMode);
    appendItem(items, "ModernReloadThemeMenuItem", _config.commands.reloadTheme);
    return flyout;
  }

  MenuFlyout ShellBuilder::nowPlayingOverflowFlyout() const
  {
    auto flyout = MenuFlyout{};
    auto const items = flyout.Items();
    appendItem(items, "NowPlayingStopMenuItem", _config.commands.stop);
    appendItem(items, "NowPlayingOpenLibraryMenuItem", _config.commands.openLibrary);
    appendItem(items, "NowPlayingRescanMenuItem", _config.commands.rescanLibrary);
    appendSeparator(items);
    appendItem(items, "NowPlayingClassicModeMenuItem", _config.commands.toggleShellMode);
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

    appendMenu("FileMenuTitle",
               [this](auto const& items)
               {
                 appendItem(items, "OpenLibraryMenuItem", _config.commands.openLibrary);
                 appendItem(items, "RescanMenuItem", _config.commands.rescanLibrary);
               });
    appendMenu("ViewMenuTitle",
               [this](auto const& items)
               {
                 appendItem(items, "ClassicColumnsMenuItem", _config.commands.chooseColumns);
                 // Classic authors no inspector toggle of its own, so the menu
                 // is the only place the overlay can be asked for at the widths
                 // that do not seat the inspector inline.
                 appendItem(items, "TrackDetailsMenuItem", _config.commands.toggleInspector);
                 appendItem(items, "ModernModeMenuItem", _config.commands.toggleShellMode);
                 appendItem(items, "ReloadThemeMenuItem", _config.commands.reloadTheme);
               });
    appendMenu("PlaybackMenuTitle",
               [this](auto const& items)
               {
                 appendItem(items, "PlayPauseMenuItem", _config.commands.playPause);
                 appendItem(items, "StopMenuItem", _config.commands.stop);
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
        .libraryTasks = runtime.library().taskService(),
        .playbackCommands = _session.playbackCommands(),
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
    text.Text(winrt::to_hstring(formatResource("ShellLayoutFailedFormat", error.message)));
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

  void ShellBuilder::retire() noexcept
  {
    if (_retired)
    {
      return;
    }

    // Close generation admission before detaching the native root. Components
    // can therefore observe retirement even if XAML refuses the detach.
    _retired = true;
    _host.retire();
    _optLivePreset.reset();
    _optRejectedPreset.reset();
  }
} // namespace ao::winui::layout

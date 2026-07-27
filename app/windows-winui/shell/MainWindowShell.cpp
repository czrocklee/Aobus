// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"
#include "app/LibrarySession.h"
#include "pch.h"
#include "platform/ScopedBooleanFlag.h"
#include "platform/WindowsStringResources.h"
#include "playback/AobusSoulControl.h"
#include "playback/PlaybackControls.h"
#include "theme/WindowsThemeCoordinator.h"
#include <ao/Error.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/layout/shell/DesktopShellPolicy.h>
#include <ao/uimodel/layout/shell/WindowsDesktopSettingsYamlSchema.h>
#include <ao/uimodel/preference/WindowsTheme.h>
#include <ao/utility/Path.h>

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.Windows.Storage.Pickers.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace winrt::Aobus::implementation
{
  namespace
  {
    using Microsoft::UI::Xaml::Visibility;

    HWND nativeWindow(Microsoft::UI::Xaml::Window const& window)
    {
      auto windowNative = window.as<::IWindowNative>();
      auto handle = HWND{};
      check_hresult(windowNative->get_WindowHandle(&handle));
      return handle;
    }

    std::vector<Windows::Foundation::IInspectable> selectionSnapshot(
      Microsoft::UI::Xaml::Controls::ListView const& list)
    {
      auto result = std::vector<Windows::Foundation::IInspectable>{};
      auto const selectedItems = list.SelectedItems();
      result.reserve(selectedItems.Size());
      for (auto const& item : selectedItems)
      {
        result.push_back(item);
      }
      return result;
    }

    void restoreSelection(Microsoft::UI::Xaml::Controls::ListView const& list,
                          std::vector<Windows::Foundation::IInspectable> const& selectedItems)
    {
      auto const target = list.SelectedItems();
      target.Clear();
      for (auto const& item : selectedItems)
      {
        target.Append(item);
      }
    }

    Microsoft::UI::Xaml::GridLength pixels(double const value) noexcept
    {
      return {
        .Value = value,
        .GridUnitType = Microsoft::UI::Xaml::GridUnitType::Pixel,
      };
    }

    Microsoft::UI::Xaml::GridLength stars(double const value = 1.0) noexcept
    {
      return {
        .Value = value,
        .GridUnitType = Microsoft::UI::Xaml::GridUnitType::Star,
      };
    }

    Windows::UI::Color parseColor(std::string_view const value)
    {
      auto const offset = value.size() == 9 ? 3U : 1U;
      auto const component = [value](std::size_t const at)
      { return static_cast<std::uint8_t>(std::stoul(std::string{value.substr(at, 2)}, nullptr, 16)); };
      return {
        .A = value.size() == 9 ? component(1) : std::uint8_t{0xFF},
        .R = component(offset),
        .G = component(offset + 2),
        .B = component(offset + 4),
      };
    }

    Microsoft::UI::Xaml::Media::SolidColorBrush colorBrush(std::string_view const value)
    {
      return Microsoft::UI::Xaml::Media::SolidColorBrush{parseColor(value)};
    }

    constexpr auto kThemeResourceKeys = std::to_array<wchar_t const*>({
      L"ContentControlThemeFontFamily",
      L"BodyFontFamily",
      L"TextFillColorPrimaryBrush",
      L"TextFillColorSecondaryBrush",
      L"TextFillColorTertiaryBrush",
      L"DividerStrokeColorDefaultBrush",
      L"AccentFillColorDefaultBrush",
      L"AccentTextFillColorPrimaryBrush",
      L"ListViewItemBackgroundSelected",
      L"ListViewItemBackgroundSelectedPointerOver",
      L"ListViewItemBackgroundSelectedPressed",
      L"ListViewItemForegroundSelected",
    });

    void setResource(Microsoft::UI::Xaml::ResourceDictionary const& resources,
                     wchar_t const* key,
                     Windows::Foundation::IInspectable const& value)
    {
      resources.Insert(box_value(key), value);
    }

    void clearThemeResources(Microsoft::UI::Xaml::ResourceDictionary const& resources)
    {
      for (auto const* key : kThemeResourceKeys)
      {
        resources.Remove(box_value(key));
      }
    }
  } // namespace

  void MainWindow::OnRootSizeChanged(Windows::Foundation::IInspectable const&,
                                     Microsoft::UI::Xaml::SizeChangedEventArgs const& args)
  {
    applyShellState(args.NewSize().Width);
  }

  void MainWindow::applyShellState(double const width)
  {
    auto const mode = _session != nullptr ? _session->settings().shellMode : ao::uimodel::DesktopShellMode::Modern;
    auto const state = ao::uimodel::DesktopShellPolicy::resolve(mode, width);
    auto const modern = state.mode == ao::uimodel::DesktopShellMode::Modern;
    auto const wasModern = ModernShell().Visibility() == Visibility::Visible;
    auto const presentationChanged = wasModern != modern;
    auto const selectedItems = presentationChanged
                                 ? selectionSnapshot(wasModern ? ModernTrackList() : ClassicTrackList())
                                 : std::vector<Windows::Foundation::IInspectable>{};
    auto const selectionChange = ao::winui::ScopedBooleanFlag{_applyingTrackSelection, presentationChanged};

    ModernShell().Visibility(modern ? Visibility::Visible : Visibility::Collapsed);
    TitleBarGrid().Visibility(modern ? Visibility::Visible : Visibility::Collapsed);
    ClassicShell().Visibility(modern ? Visibility::Collapsed : Visibility::Visible);
    if (presentationChanged)
    {
      restoreSelection(ModernTrackList(), selectedItems);
      restoreSelection(ClassicTrackList(), selectedItems);
    }
    get_self<AobusSoulControl>(ModernSoul())->setPresentationActive(modern);
    get_self<AobusSoulControl>(ClassicSoul())->setPresentationActive(!modern);
    if (_playbackControlsPtr)
    {
      _playbackControlsPtr->setPresentationActive(modern);
    }
    TitleBarRow().Height(pixels(modern ? 40.0 : 0.0));
    ExtendsContentIntoTitleBar(state.integratedTitleBar);
    SetTitleBar(state.integratedTitleBar ? TitleBarGrid() : Microsoft::UI::Xaml::UIElement{nullptr});

    using Navigation = ao::uimodel::DesktopNavigationPresentation;
    using Inspector = ao::uimodel::DesktopInspectorPresentation;
    auto const navigationWidth =
      _session != nullptr ? _session->settings().navigationPaneWidth : ao::uimodel::kDefaultWindowsNavigationPaneWidth;
    auto const inspectorWidth =
      _session != nullptr ? _session->settings().inspectorPaneWidth : ao::uimodel::kDefaultWindowsInspectorPaneWidth;
    ModernNavigation().OpenPaneLength(navigationWidth);
    if (state.navigation == Navigation::Expanded)
    {
      ModernNavigation().PaneDisplayMode(Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::Left);
      ModernNavigation().IsPaneOpen(true);
    }
    else if (state.navigation == Navigation::Compact)
    {
      ModernNavigation().PaneDisplayMode(Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::LeftCompact);
      ModernNavigation().IsPaneOpen(false);
    }
    else
    {
      ModernNavigation().PaneDisplayMode(Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::LeftMinimal);
      ModernNavigation().IsPaneOpen(false);
    }

    auto const inlineInspector = state.inspector == Inspector::Inline;
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(ModernInspector(), inlineInspector ? 1 : 0);
    Microsoft::UI::Xaml::Controls::Canvas::SetZIndex(ModernInspector(), inlineInspector ? 0 : 10);
    ModernInspector().HorizontalAlignment(inlineInspector ? Microsoft::UI::Xaml::HorizontalAlignment::Stretch
                                                          : Microsoft::UI::Xaml::HorizontalAlignment::Right);
    auto const inspectorCardWidth = std::max(0.0, inspectorWidth - 12.0);
    auto const inspectorCoverSide = std::clamp(inspectorCardWidth - 34.0, 0.0, 320.0);
    ModernInspectorColumn().Width(pixels(inlineInspector ? inspectorWidth : 0.0));
    ModernInspector().Width(inspectorCardWidth);
    InspectorCoverSlot().Width(inspectorCoverSide);
    InspectorCoverSlot().Height(inspectorCoverSide);
    ModernInspector().Visibility(inlineInspector || _inspectorRequested ? Visibility::Visible : Visibility::Collapsed);
    ModernNavigationSplitter().Visibility(modern && state.navigation == Navigation::Expanded ? Visibility::Visible
                                                                                             : Visibility::Collapsed);
    ModernInspectorSplitter().Visibility(modern && inlineInspector ? Visibility::Visible : Visibility::Collapsed);

    auto const wide = state.widthClass == ao::uimodel::DesktopShellWidthClass::Wide;
    auto const narrow = state.widthClass == ao::uimodel::DesktopShellWidthClass::Narrow;
    ModernFilterColumn().Width(wide ? pixels(420.0) : stars());
    ModernSummaryColumn().Width(wide ? stars() : pixels(0.0));
    ModernBrowserSummary().Visibility(wide ? Visibility::Visible : Visibility::Collapsed);
    ModernNowPlayingInfoColumn().Width(narrow ? pixels(0.0) : stars(2.0));
    ModernNowPlayingInfo().Visibility(narrow ? Visibility::Collapsed : Visibility::Visible);
    ModernNowPlayingRightColumn().Width(stars(narrow ? 1.0 : 2.0));

    auto const classicSidebars = state.widthClass != ao::uimodel::DesktopShellWidthClass::Narrow;
    ClassicNavigationColumn().Width(pixels(classicSidebars ? navigationWidth : 0.0));
    ClassicLibraryTree().Visibility(classicSidebars ? Visibility::Visible : Visibility::Collapsed);
    ClassicInspectorColumn().Width(pixels(state.inspector == Inspector::Inline ? inspectorWidth : 0.0));
    ClassicInspector().Visibility(state.inspector == Inspector::Inline ? Visibility::Visible : Visibility::Collapsed);
    ClassicNavigationSplitter().Visibility(!modern && classicSidebars ? Visibility::Visible : Visibility::Collapsed);
    ClassicInspectorSplitter().Visibility(!modern && inlineInspector ? Visibility::Visible : Visibility::Collapsed);
  }

  void MainWindow::OnPaneResizeDelta(Windows::Foundation::IInspectable const& sender,
                                     Microsoft::UI::Xaml::Controls::Primitives::DragDeltaEventArgs const& args)
  {
    if (_session == nullptr)
    {
      return;
    }

    auto const element = sender.try_as<Microsoft::UI::Xaml::FrameworkElement>();
    if (!element)
    {
      return;
    }

    auto const kind = to_string(unbox_value_or<hstring>(element.Tag(), L""));
    auto& settings = _session->settings();
    if (kind == "navigation")
    {
      settings.navigationPaneWidth = std::clamp(settings.navigationPaneWidth + args.HorizontalChange(),
                                                ao::uimodel::kMinimumWindowsNavigationPaneWidth,
                                                ao::uimodel::kMaximumWindowsNavigationPaneWidth);
    }
    else if (kind == "inspector")
    {
      settings.inspectorPaneWidth = std::clamp(settings.inspectorPaneWidth - args.HorizontalChange(),
                                               ao::uimodel::kMinimumWindowsInspectorPaneWidth,
                                               ao::uimodel::kMaximumWindowsInspectorPaneWidth);
    }
    else
    {
      return;
    }

    _paneResizeDirty = true;
    applyShellState(RootGrid().ActualWidth());
  }

  void MainWindow::OnPaneResizeCompleted(Windows::Foundation::IInspectable const&,
                                         Microsoft::UI::Xaml::Controls::Primitives::DragCompletedEventArgs const&)
  {
    if (!_paneResizeDirty)
    {
      return;
    }

    _paneResizeDirty = false;
    saveWindowState();
  }

  void MainWindow::restoreWindowPlacement()
  {
    auto const& placement = _session->settings().window;
    auto const right = static_cast<std::int64_t>(placement.x) + placement.width;
    auto const bottom = static_cast<std::int64_t>(placement.y) + placement.height;
    constexpr auto kMinimumLong = static_cast<std::int64_t>(std::numeric_limits<LONG>::min());
    constexpr auto kMaximumLong = static_cast<std::int64_t>(std::numeric_limits<LONG>::max());
    if (right < kMinimumLong || right > kMaximumLong || bottom < kMinimumLong || bottom > kMaximumLong)
    {
      APP_LOG_WARN("MainWindow: rejected native window placement with overflowing bounds");
      return;
    }

    auto nativePlacement = WINDOWPLACEMENT{};
    nativePlacement.length = sizeof(nativePlacement);
    nativePlacement.showCmd = placement.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    nativePlacement.rcNormalPosition = {
      .left = placement.x,
      .top = placement.y,
      .right = static_cast<LONG>(right),
      .bottom = static_cast<LONG>(bottom),
    };

    if (::SetWindowPlacement(nativeWindow(*this), &nativePlacement) == FALSE)
    {
      APP_LOG_WARN("MainWindow: failed to restore native window placement: {}", ::GetLastError());
    }
  }

  void MainWindow::saveWindowState()
  {
    if (_session == nullptr)
    {
      return;
    }

    auto& placement = _session->settings().window;
    auto nativePlacement = WINDOWPLACEMENT{};
    nativePlacement.length = sizeof(nativePlacement);

    if (::GetWindowPlacement(nativeWindow(*this), &nativePlacement) != FALSE)
    {
      auto const& normal = nativePlacement.rcNormalPosition;
      placement.x = normal.left;
      placement.y = normal.top;
      placement.width = normal.right - normal.left;
      placement.height = normal.bottom - normal.top;
      placement.maximized =
        nativePlacement.showCmd == SW_SHOWMAXIMIZED || (nativePlacement.flags & WPF_RESTORETOMAXIMIZED) != 0;
    }
    else
    {
      APP_LOG_WARN("MainWindow: failed to read native window placement: {}", ::GetLastError());
    }

    if (auto const saved = _session->saveSettings(); !saved)
    {
      updateStatus(ao::winui::formatResource("SaveSettingsFailedFormat", saved.error().message));
    }
  }

  winrt::fire_and_forget MainWindow::pickLibrary()
  {
    auto lifetime = get_strong();
    try
    {
      auto picker = Microsoft::Windows::Storage::Pickers::FolderPicker{AppWindow().Id()};
      picker.SuggestedStartLocation(Microsoft::Windows::Storage::Pickers::PickerLocationId::MusicLibrary);
      picker.CommitButtonText(ao::winui::resourceHstring(L"OpenLibraryPickerButton"));
      auto result = co_await picker.PickSingleFolderAsync();
      if (result && !result.Path().empty() && _session != nullptr)
      {
        _session->openLibrary(std::filesystem::path{result.Path().c_str()});
      }
    }
    catch (hresult_error const& error)
    {
      updateStatus(ao::winui::formatResource("FolderPickerFailedFormat", to_string(error.message())));
    }
  }

  void MainWindow::OnOpenLibraryClicked(Windows::Foundation::IInspectable const&,
                                        Microsoft::UI::Xaml::RoutedEventArgs const&)
  {
    pickLibrary();
  }

  void MainWindow::OnRescanClicked(Windows::Foundation::IInspectable const&,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&)
  {
    if (_session != nullptr)
    {
      _session->rescan();
    }
  }

  void MainWindow::OnToggleModeClicked(Windows::Foundation::IInspectable const&,
                                       Microsoft::UI::Xaml::RoutedEventArgs const&)
  {
    if (_session == nullptr)
    {
      return;
    }
    auto& settings = _session->settings();
    settings.shellMode = settings.shellMode == ao::uimodel::DesktopShellMode::Modern
                           ? ao::uimodel::DesktopShellMode::Classic
                           : ao::uimodel::DesktopShellMode::Modern;
    applyShellState(RootGrid().ActualWidth());
    saveWindowState();
  }

  void MainWindow::OnReloadThemeClicked(Windows::Foundation::IInspectable const&,
                                        Microsoft::UI::Xaml::RoutedEventArgs const&)
  {
    if (!_themePtr)
    {
      return;
    }
    auto reloaded = _themePtr->reload();
    if (!reloaded)
    {
      if (reloaded.error().code == ao::Error::Code::NotFound)
      {
        applySystemTheme();
        updateStatus(ao::winui::resourceString("ThemeOverrideRemoved"));
        return;
      }
      updateStatus(ao::winui::formatResource("ThemeReloadFailedFormat", reloaded.error().message));
      return;
    }
    applyTheme(*reloaded);
    updateStatus(ao::winui::formatResource("ThemeReloadedFormat", ao::utility::pathToUtf8(_themePtr->path())));
  }

  void MainWindow::applyTheme(ao::uimodel::WindowsTheme const& theme)
  {
    auto const resources = RootGrid().Resources();
    clearThemeResources(resources);
    auto const primary = colorBrush(theme.shared.textPrimary);
    auto const secondary = colorBrush(theme.shared.textSecondary);
    auto const divider = colorBrush(theme.shared.divider);
    auto const accent = colorBrush(theme.shared.accent);
    auto const selection = colorBrush(theme.shared.selection);
    auto const surface = colorBrush(theme.shared.surface);
    auto const font = Microsoft::UI::Xaml::Media::FontFamily{to_hstring(theme.shared.fontFamily)};

    setResource(resources, L"ContentControlThemeFontFamily", font);
    setResource(resources, L"BodyFontFamily", font);
    setResource(resources, L"TextFillColorPrimaryBrush", primary);
    setResource(resources, L"TextFillColorSecondaryBrush", secondary);
    setResource(resources, L"TextFillColorTertiaryBrush", secondary);
    setResource(resources, L"DividerStrokeColorDefaultBrush", divider);
    setResource(resources, L"AccentFillColorDefaultBrush", accent);
    setResource(resources, L"AccentTextFillColorPrimaryBrush", accent);
    setResource(resources, L"ListViewItemBackgroundSelected", selection);
    setResource(resources, L"ListViewItemBackgroundSelectedPointerOver", selection);
    setResource(resources, L"ListViewItemBackgroundSelectedPressed", selection);
    setResource(resources, L"ListViewItemForegroundSelected", primary);

    RootGrid().Background(colorBrush(theme.shared.windowBackground));
    ModernNavigation().Background(colorBrush(theme.modern.navigationBackground));
    ModernInspector().Background(colorBrush(theme.modern.inspectorBackground));
    ModernNowPlaying().Background(colorBrush(theme.modern.nowPlayingBackground));
    ModernTrackSurface().Background(surface);
    ModernTrackList().Background(surface);
    ModernColumnHeaders().Background(surface);
    ClassicShell().Background(surface);
    ClassicTrackSurface().Background(surface);
    ClassicTrackList().Background(surface);
    ClassicToolbar().Background(colorBrush(theme.classic.toolbarBackground));
    ClassicLibraryTree().Background(colorBrush(theme.classic.treeBackground));
    ClassicInspector().Background(colorBrush(theme.classic.treeBackground));
    ClassicColumnHeaders().Background(colorBrush(theme.classic.toolbarBackground));
    ClassicStatusBar().Background(colorBrush(theme.classic.statusBackground));

    auto const retro = theme.classic.chrome == ao::uimodel::WindowsClassicChrome::Retro;
    ClassicToolbar().CornerRadius(retro ? Microsoft::UI::Xaml::CornerRadius{0.0}
                                        : Microsoft::UI::Xaml::CornerRadius{4.0});
    ClassicStatusBar().CornerRadius(retro ? Microsoft::UI::Xaml::CornerRadius{0.0}
                                          : Microsoft::UI::Xaml::CornerRadius{4.0});
  }

  void MainWindow::applySystemTheme()
  {
    using Microsoft::UI::Xaml::Application;
    using Microsoft::UI::Xaml::Media::Brush;

    clearThemeResources(RootGrid().Resources());
    auto const applicationResources = Application::Current().Resources();
    auto const brush = [&applicationResources](wchar_t const* key)
    { return applicationResources.Lookup(box_value(key)).as<Brush>(); };

    RootGrid().Background(nullptr);
    ModernNavigation().ClearValue(Microsoft::UI::Xaml::Controls::Control::BackgroundProperty());
    ModernInspector().Background(brush(L"CardBackgroundFillColorDefaultBrush"));
    ModernNowPlaying().Background(brush(L"CardBackgroundFillColorDefaultBrush"));
    ModernTrackSurface().Background(nullptr);
    ModernTrackList().ClearValue(Microsoft::UI::Xaml::Controls::Control::BackgroundProperty());
    ModernColumnHeaders().ClearValue(Microsoft::UI::Xaml::Controls::Control::BackgroundProperty());
    ClassicShell().Background(nullptr);
    ClassicTrackSurface().Background(nullptr);
    ClassicTrackList().ClearValue(Microsoft::UI::Xaml::Controls::Control::BackgroundProperty());
    ClassicToolbar().Background(brush(L"CardBackgroundFillColorDefaultBrush"));
    ClassicLibraryTree().Background(brush(L"ApplicationPageBackgroundThemeBrush"));
    ClassicInspector().Background(brush(L"ApplicationPageBackgroundThemeBrush"));
    ClassicColumnHeaders().ClearValue(Microsoft::UI::Xaml::Controls::Control::BackgroundProperty());
    ClassicStatusBar().Background(brush(L"CardBackgroundFillColorDefaultBrush"));
    ClassicToolbar().CornerRadius({4.0});
    ClassicStatusBar().CornerRadius({4.0});
  }

  void MainWindow::updateStatus(std::string const& status)
  {
    StatusText().Text(to_hstring(status));
  }
} // namespace winrt::Aobus::implementation

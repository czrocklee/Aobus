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

#include <errhandlingapi.h>
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

    constexpr std::size_t kArgbColorLength = 9;
    constexpr std::uint8_t kOpaqueAlpha = 0xFF;
    constexpr double kModernTitleBarHeight = 40.0;
    constexpr std::int32_t kOverlayInspectorZIndex = 10;
    constexpr double kWideFilterWidth = 420.0;
    constexpr double kNowPlayingColumnWeight = 2.0;
    constexpr double kClassicCornerRadius = 4.0;

    HWND nativeWindow(Microsoft::UI::Xaml::Window const& window)
    {
      auto windowNative = window.as<::IWindowNative>();
      HWND handle = nullptr;
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
      auto const offset = value.size() == kArgbColorLength ? 3U : 1U;
      auto const component = [value](std::size_t const at)
      { return static_cast<std::uint8_t>(std::stoul(std::string{value.substr(at, 2)}, nullptr, 16)); };
      return {
        .A = value.size() == kArgbColorLength ? component(1) : kOpaqueAlpha,
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

  void MainWindow::OnRootSizeChanged(Windows::Foundation::IInspectable const& /*sender*/,
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
    auto const navigationWidth =
      _session != nullptr ? _session->settings().navigationPaneWidth : ao::uimodel::kDefaultWindowsNavigationPaneWidth;
    auto const inspectorWidth =
      _session != nullptr ? _session->settings().inspectorPaneWidth : ao::uimodel::kDefaultWindowsInspectorPaneWidth;
    applyShellModePresentation(state, modern, presentationChanged, selectedItems);
    applyNavigationPresentation(state, modern, navigationWidth);
    applyInspectorPresentation(state, modern, inspectorWidth);
    applyResponsiveLayout(state, navigationWidth, inspectorWidth);
  }

  void MainWindow::applyShellModePresentation(ao::uimodel::DesktopShellViewState const& state,
                                              bool const modern,
                                              bool const presentationChanged,
                                              std::vector<Windows::Foundation::IInspectable> const& selectedItems)
  {
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

    TitleBarRow().Height(pixels(modern ? kModernTitleBarHeight : 0.0));
    ExtendsContentIntoTitleBar(state.integratedTitleBar);
    SetTitleBar(state.integratedTitleBar ? TitleBarGrid() : Microsoft::UI::Xaml::UIElement{nullptr});
  }

  void MainWindow::applyNavigationPresentation(ao::uimodel::DesktopShellViewState const& state,
                                               bool const modern,
                                               double const navigationWidth)
  {
    using Navigation = ao::uimodel::DesktopNavigationPresentation;
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

    ModernNavigationSplitter().Visibility(modern && state.navigation == Navigation::Expanded ? Visibility::Visible
                                                                                             : Visibility::Collapsed);
  }

  void MainWindow::applyInspectorPresentation(ao::uimodel::DesktopShellViewState const& state,
                                              bool const modern,
                                              double const inspectorWidth)
  {
    using Inspector = ao::uimodel::DesktopInspectorPresentation;
    auto const inlineInspector = state.inspector == Inspector::Inline;
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(ModernInspector(), inlineInspector ? 1 : 0);
    Microsoft::UI::Xaml::Controls::Canvas::SetZIndex(ModernInspector(), inlineInspector ? 0 : kOverlayInspectorZIndex);
    ModernInspector().HorizontalAlignment(inlineInspector ? Microsoft::UI::Xaml::HorizontalAlignment::Stretch
                                                          : Microsoft::UI::Xaml::HorizontalAlignment::Right);
    auto const inspectorCardWidth = std::max(0.0, inspectorWidth - 12.0);
    auto const inspectorCoverSide = std::clamp(inspectorCardWidth - 34.0, 0.0, 320.0);
    ModernInspectorColumn().Width(pixels(inlineInspector ? inspectorWidth : 0.0));
    ModernInspector().Width(inspectorCardWidth);
    InspectorCoverSlot().Width(inspectorCoverSide);
    InspectorCoverSlot().Height(inspectorCoverSide);
    ModernInspector().Visibility(inlineInspector || _inspectorRequested ? Visibility::Visible : Visibility::Collapsed);
    ModernInspectorSplitter().Visibility(modern && inlineInspector ? Visibility::Visible : Visibility::Collapsed);
  }

  void MainWindow::applyResponsiveLayout(ao::uimodel::DesktopShellViewState const& state,
                                         double const navigationWidth,
                                         double const inspectorWidth)
  {
    using Inspector = ao::uimodel::DesktopInspectorPresentation;
    auto const modern = state.mode == ao::uimodel::DesktopShellMode::Modern;
    auto const inlineInspector = state.inspector == Inspector::Inline;
    auto const wide = state.widthClass == ao::uimodel::DesktopShellWidthClass::Wide;
    auto const narrow = state.widthClass == ao::uimodel::DesktopShellWidthClass::Narrow;
    ModernFilterColumn().Width(wide ? pixels(kWideFilterWidth) : stars());
    ModernSummaryColumn().Width(wide ? stars() : pixels(0.0));
    ModernBrowserSummary().Visibility(wide ? Visibility::Visible : Visibility::Collapsed);
    ModernNowPlayingInfoColumn().Width(narrow ? pixels(0.0) : stars(kNowPlayingColumnWeight));
    ModernNowPlayingInfo().Visibility(narrow ? Visibility::Collapsed : Visibility::Visible);
    ModernNowPlayingRightColumn().Width(stars(narrow ? 1.0 : kNowPlayingColumnWeight));

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

    if (kind == "navigation")
    {
      auto& settings = _session->settings();
      settings.navigationPaneWidth = std::clamp(settings.navigationPaneWidth + args.HorizontalChange(),
                                                ao::uimodel::kMinimumWindowsNavigationPaneWidth,
                                                ao::uimodel::kMaximumWindowsNavigationPaneWidth);
    }
    else if (kind == "inspector")
    {
      auto& settings = _session->settings();
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

  void MainWindow::OnPaneResizeCompleted(
    Windows::Foundation::IInspectable const& /*sender*/,
    Microsoft::UI::Xaml::Controls::Primitives::DragCompletedEventArgs const& /*args*/)
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

      if (auto result = co_await picker.PickSingleFolderAsync();
          result && !result.Path().empty() && _session != nullptr)
      {
        _session->openLibrary(std::filesystem::path{result.Path().c_str()});
      }
    }
    catch (hresult_error const& error)
    {
      updateStatus(ao::winui::formatResource("FolderPickerFailedFormat", to_string(error.message())));
    }
  }

  void MainWindow::OnOpenLibraryClicked(Windows::Foundation::IInspectable const& /*sender*/,
                                        Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    pickLibrary();
  }

  void MainWindow::OnRescanClicked(Windows::Foundation::IInspectable const& /*sender*/,
                                   Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    if (_session != nullptr)
    {
      _session->rescan();
    }
  }

  void MainWindow::OnToggleModeClicked(Windows::Foundation::IInspectable const& /*sender*/,
                                       Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
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

  void MainWindow::OnReloadThemeClicked(Windows::Foundation::IInspectable const& /*sender*/,
                                        Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
  {
    if (_themePtr == nullptr)
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
    auto const cornerRadius = retro ? 0.0 : kClassicCornerRadius;
    ClassicToolbar().CornerRadius({
      .TopLeft = cornerRadius,
      .TopRight = cornerRadius,
      .BottomRight = cornerRadius,
      .BottomLeft = cornerRadius,
    });
    ClassicStatusBar().CornerRadius({
      .TopLeft = cornerRadius,
      .TopRight = cornerRadius,
      .BottomRight = cornerRadius,
      .BottomLeft = cornerRadius,
    });
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
    ClassicToolbar().CornerRadius({
      .TopLeft = kClassicCornerRadius,
      .TopRight = kClassicCornerRadius,
      .BottomRight = kClassicCornerRadius,
      .BottomLeft = kClassicCornerRadius,
    });
    ClassicStatusBar().CornerRadius({
      .TopLeft = kClassicCornerRadius,
      .TopRight = kClassicCornerRadius,
      .BottomRight = kClassicCornerRadius,
      .BottomLeft = kClassicCornerRadius,
    });
  }

  void MainWindow::updateStatus(std::string const& status)
  {
    StatusText().Text(to_hstring(status));
  }
} // namespace winrt::Aobus::implementation

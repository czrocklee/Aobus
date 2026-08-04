// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MainWindow.xaml.h"
#include "app/LibrarySession.h"
#include "layout/ShellBuilder.h"
#include "pch.h"
#include "platform/StringResources.h"
#include "theme/SurfaceBrushes.h"
#include "theme/ThemeCoordinator.h"
#include <ao/Error.h>
#include <ao/rt/Log.h>
#include <ao/utility/Path.h>
#include <ao/winui/Theme.h>
#include <ao/winui/layout/ShellStatePolicy.h>

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

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

namespace winrt::Aobus::implementation
{
  namespace
  {
    constexpr double kClassicChromeCornerRadius = 4.0;

    HWND nativeWindow(Microsoft::UI::Xaml::Window const& window)
    {
      auto windowNative = window.as<::IWindowNative>();
      HWND handle = nullptr;
      check_hresult(windowNative->get_WindowHandle(&handle));
      return handle;
    }

    Microsoft::UI::Xaml::Media::SolidColorBrush colorBrush(std::string_view const value)
    {
      return ao::winui::themeColorBrush(value);
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

    /**
     * @brief Replace the rounding the Classic chrome styles resolve.
     *
     * The key is declared in the window resources rather than removed and
     * restored: a `ThemeResource` that resolves to nothing throws when the
     * style is applied, and every rebuild applies these styles again.
     */
    void setClassicChromeCornerRadius(Microsoft::UI::Xaml::ResourceDictionary const& resources, double const radius)
    {
      resources.Insert(box_value(L"ClassicChromeCornerRadius"),
                       box_value(Microsoft::UI::Xaml::CornerRadius{
                         .TopLeft = radius, .TopRight = radius, .BottomRight = radius, .BottomLeft = radius}));
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
    // The window is sized before it has a session to build a shell from, so the
    // first resolved policy is the one `initialize` asks for.
    if (!_shellBuilderPtr)
    {
      return;
    }

    auto const mode = _session != nullptr ? _session->settings().shellMode : ao::winui::ShellMode::Modern;
    auto applied = _shellBuilderPtr->applyShellState(mode, width, _optInspectorRequest);

    if (!applied)
    {
      // The generation that was already live stays live, which is the whole
      // point of building the candidate before publishing it.
      updateStatus(ao::winui::formatResource("ShellLayoutFailedFormat", applied.error().message));
      return;
    }

    auto const& state = *applied;
    ExtendsContentIntoTitleBar(state.integratedTitleBar);
    SetTitleBar(state.integratedTitleBar ? _shellBuilderPtr->titleBar() : Microsoft::UI::Xaml::UIElement{nullptr});
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

  void MainWindow::saveWindowState() noexcept
  {
    if (_session == nullptr)
    {
      return;
    }

    try
    {
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
    }
    catch (std::exception const& error)
    {
      APP_LOG_WARN("MainWindow: failed to capture native window placement: {}", error.what());
    }
    catch (...)
    {
      APP_LOG_WARN("MainWindow: failed to capture native window placement: unknown exception");
    }

    try
    {
      if (auto const saved = _session->saveSettings(); !saved)
      {
        try
        {
          updateStatus(ao::winui::formatResource("SaveSettingsFailedFormat", saved.error().message));
        }
        catch (...)
        {
          APP_LOG_WARN("MainWindow: failed to report settings checkpoint failure");
        }
      }
    }
    catch (std::exception const& error)
    {
      APP_LOG_WARN("MainWindow: settings checkpoint failed: {}", error.what());
    }
    catch (...)
    {
      APP_LOG_WARN("MainWindow: settings checkpoint failed: unknown exception");
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
          result && !result.Path().empty() && _session != nullptr &&
          (_sessionPhase == SessionPhase::Prepared || _sessionPhase == SessionPhase::Active) && _requestRestart)
      {
        // The callback only queues an App-owned restart. Its dispatcher turn
        // runs after this coroutine returns and releases its strong window ref.
        if (auto const requested = _requestRestart(std::filesystem::path{result.Path().c_str()}); !requested)
        {
          updateStatus(ao::winui::formatResource("ErrorFormat", requested.error().message));
        }
      }
    }
    catch (hresult_error const& error)
    {
      updateStatus(ao::winui::formatResource("FolderPickerFailedFormat", to_string(error.message())));
    }
  }

  void MainWindow::rescanLibrary()
  {
    if (_session != nullptr)
    {
      _session->rescan();
    }
  }

  void MainWindow::toggleShellMode()
  {
    if (_session == nullptr)
    {
      return;
    }

    auto& settings = _session->settings();
    settings.shellMode =
      settings.shellMode == ao::winui::ShellMode::Modern ? ao::winui::ShellMode::Classic : ao::winui::ShellMode::Modern;
    applyShellState(RootGrid().ActualWidth());
    saveWindowState();
  }

  void MainWindow::reloadTheme()
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

  /**
   * @brief Rebuild the document-built shell against the theme now in force.
   *
   * A preset's `surface` slots are painted from the override at construction,
   * so unlike a `ThemeResource` they do not re-resolve on their own. The whole
   * generation is rebuilt rather than repainted: that is the one path a preset
   * change is already known to survive.
   */
  void MainWindow::rebuildForTheme()
  {
    if (!_shellBuilderPtr)
    {
      return;
    }

    if (auto rebuilt = _shellBuilderPtr->rebuild(); !rebuilt)
    {
      updateStatus(ao::winui::formatResource("ShellLayoutFailedFormat", rebuilt.error().message));
    }
  }

  void MainWindow::applyTheme(ao::winui::Theme const& theme)
  {
    _themeOverride = theme;
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

    // A preset's own chrome takes its rounding from the window resources, so a
    // retro theme squares the Classic bars off without the frame knowing which
    // elements those bars are.
    auto const retro = theme.classic.chrome == ao::winui::ClassicChrome::Retro;
    setClassicChromeCornerRadius(resources, retro ? 0.0 : kClassicChromeCornerRadius);
    rebuildForTheme();
  }

  void MainWindow::applySystemTheme()
  {
    _themeOverride.reset();
    auto const resources = RootGrid().Resources();
    clearThemeResources(resources);
    RootGrid().Background(nullptr);
    setClassicChromeCornerRadius(resources, kClassicChromeCornerRadius);
    rebuildForTheme();
  }

  void MainWindow::updateStatus(std::string const& status)
  {
    // The shell retains the message, so a generation built later still shows it.
    if (_shellBuilderPtr)
    {
      _shellBuilderPtr->reportStatus(status);
    }
  }
} // namespace winrt::Aobus::implementation

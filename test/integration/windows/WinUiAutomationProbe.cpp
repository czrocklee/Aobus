// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Audit resolved native UIA names on the loaded shipping shells, not spelling
// of SetName calls. Canary controls prove that bypassing makeNamed is detected.
#include "MainWindow.xaml.h"
#include "WinUiSilentAudio.h"
#include "XamlMetaDataProvider.h"
#include "app/LibrarySession.h"
#include "pch.h"
#include "platform/NamedControl.h"
#include "platform/StringResources.h"
#include <ao/Error.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/i18n/IcuCompletionAliases.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionAliasPolicy.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/utility/ScopedRegistration.h>
#include <ao/winui/layout/ShellState.h>

#include <commctrl.h>
#include <microsoft.ui.xaml.window.h>
#include <unicode/uchar.h>
#include <unicode/umachine.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
  using namespace winrt::Microsoft::UI::Xaml;
  using Item = winrt::Windows::Foundation::IInspectable;
  using Automation::AutomationProperties;
  using Automation::Peers::AutomationControlType;
  using Automation::Peers::AutomationPeer;
  using Automation::Peers::FrameworkElementAutomationPeer;
  using Automation::Peers::PatternInterface;
  using Controls::Button;
  using Controls::Flyout;
  using Controls::Grid;
  using Controls::Primitives::FlyoutBase;
  using Controls::Primitives::Thumb;

  static_assert(std::same_as<decltype(ao::winui::makeNamed<Button>(L"Name")), Button>);
  static_assert(std::same_as<decltype(ao::winui::makeNamed<Controls::Slider>(L"Name")), Controls::Slider>);
  static_assert(std::same_as<decltype(ao::winui::makeNamed<Thumb>(L"Name")), Thumb>);

  bool hasInteractiveUserToken()
  {
    ::HANDLE token = nullptr;

    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0)
    {
      return false;
    }

    auto owner = ao::utility::ScopedRegistration{[token] { ::CloseHandle(token); }};
    ::DWORD bytes = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    auto storage = std::vector<std::byte>(bytes);

    if (::GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes) == 0)
    {
      return false;
    }

    auto const* user = reinterpret_cast<::TOKEN_USER const*>(storage.data());
    return ::IsWellKnownSid(user->User.Sid, WinLocalSystemSid) == 0 &&
           ::IsWellKnownSid(user->User.Sid, WinLocalServiceSid) == 0 &&
           ::IsWellKnownSid(user->User.Sid, WinNetworkServiceSid) == 0;
  }

  bool hasWords(winrt::hstring const& text)
  {
    return std::ranges::any_of(
      text, [](wchar_t const character) { return !::u_isUWhiteSpace(static_cast<::UChar32>(character)); });
  }

  bool isNameRequired(AutomationControlType const role)
  {
    switch (role)
    {
      case AutomationControlType::Button:
      case AutomationControlType::Calendar:
      case AutomationControlType::CheckBox:
      case AutomationControlType::ComboBox:
      case AutomationControlType::DataItem:
      case AutomationControlType::Edit:
      case AutomationControlType::HeaderItem:
      case AutomationControlType::Hyperlink:
      case AutomationControlType::ListItem:
      case AutomationControlType::MenuItem:
      case AutomationControlType::RadioButton:
      case AutomationControlType::Slider:
      case AutomationControlType::Spinner:
      case AutomationControlType::SplitButton:
      case AutomationControlType::TabItem:
      case AutomationControlType::Thumb:
      case AutomationControlType::TreeItem: return true;
      default: return false;
    }
  }

  struct AuditedControl final
  {
    AutomationControlType role;
    std::string id;
    std::string name;
    std::vector<std::string> scopes;
    bool offscreen;
  };

  struct Audit final
  {
    std::vector<AuditedControl> controls;
    std::vector<std::string> missing;
  };

  bool hasControl(Audit const& audit,
                  AutomationControlType const role,
                  std::string_view const scope,
                  std::string_view const name = {})
  {
    return std::ranges::any_of(
      audit.controls,
      [=](AuditedControl const& control)
      {
        return control.role == role && !control.offscreen &&
               (control.id == scope || std::ranges::find(control.scopes, scope) != control.scopes.end()) &&
               (name.empty() || control.name == name);
      });
  }

  void auditPeer(AutomationPeer const& peer,
                 std::string const& parent,
                 Audit& result,
                 std::vector<std::string> const& inheritedScopes = {})
  {
    auto const role = peer.GetAutomationControlType();
    auto const id = peer.GetAutomationId();
    auto const path = std::format("{}/{}[role={},id={}]",
                                  parent,
                                  winrt::to_string(peer.GetClassName()),
                                  static_cast<std::int32_t>(role),
                                  winrt::to_string(id));
    // ScrollBar's template buttons are implementation details of its native
    // scrolling provider, not independently authored application commands.
    if (role == AutomationControlType::ScrollBar)
    {
      return;
    }

    auto scopes = inheritedScopes;

    // Visual ancestors supply only scope metadata for a peer already reached
    // by the audit. They never establish that a control was exposed by UIA.
    if (auto const nativePeer = peer.try_as<FrameworkElementAutomationPeer>(); nativePeer)
    {
      for (DependencyObject owner = nativePeer.Owner(); owner; owner = Media::VisualTreeHelper::GetParent(owner))
      {
        auto const ownerId = AutomationProperties::GetAutomationId(owner);

        if (!ownerId.empty())
        {
          scopes.push_back(winrt::to_string(ownerId));
        }
      }
    }

    if (!id.empty())
    {
      scopes.push_back(winrt::to_string(id));
    }

    if (peer.IsControlElement() && isNameRequired(role))
    {
      auto const name = peer.GetName();
      result.controls.push_back({.role = role,
                                 .id = winrt::to_string(id),
                                 .name = winrt::to_string(name),
                                 .scopes = scopes,
                                 .offscreen = peer.IsOffscreen()});

      if (!hasWords(name))
      {
        result.missing.push_back(path);
      }
    }

    if (auto const children = peer.GetChildren(); children)
    {
      for (auto const& child : children)
      {
        auditPeer(child, path, result, scopes);
      }
    }
  }

  void auditVisual(DependencyObject const& element, std::string const& parent, Audit& result)
  {
    if (auto const native = element.try_as<UIElement>(); native)
    {
      if (native.Visibility() == Visibility::Collapsed)
      {
        return;
      }

      if (auto const peer = FrameworkElementAutomationPeer::CreatePeerForElement(native); peer)
      {
        auditPeer(peer, parent, result);
        return;
      }
    }

    for (std::int32_t index = 0; index < Media::VisualTreeHelper::GetChildrenCount(element); ++index)
    {
      auditVisual(Media::VisualTreeHelper::GetChild(element, index), parent, result);
    }
  }

  template<typename T>
  T findVisual(DependencyObject const& root, winrt::hstring const& name)
  {
    if (auto const element = root.try_as<T>(); element)
    {
      auto const peer = FrameworkElementAutomationPeer::CreatePeerForElement(element);

      if (peer && peer.GetName() == name)
      {
        return element;
      }
    }

    for (std::int32_t index = 0; index < Media::VisualTreeHelper::GetChildrenCount(root); ++index)
    {
      if (auto const found = findVisual<T>(Media::VisualTreeHelper::GetChild(root, index), name); found)
      {
        return found;
      }
    }

    return nullptr;
  }

  ao::Result<> seedLibrary(std::filesystem::path const& root)
  {
    auto const databasePath = ao::rt::LibraryPaths{root}.databasePath();
    std::filesystem::create_directories(databasePath);
    auto libraryRes = ao::library::MusicLibrary::open(root, databasePath);

    if (!libraryRes)
    {
      return std::unexpected{libraryRes.error()};
    }

    auto writableRes = ao::library::WritableMusicLibrary::acquire(*libraryRes);

    if (!writableRes)
    {
      return std::unexpected{writableRes.error()};
    }

    auto transaction = writableRes->writeTransaction();
    auto applyRes = transaction.apply(
      [](ao::library::LibraryWrite& write) -> ao::Result<>
      {
        for (auto const* title : {"Audit Alpha", "Audit Beta"})
        {
          auto builder = ao::library::TrackBuilder::makeEmpty();
          builder.metadata().title(title).artist("Audit Artist").album("Audit Album");
          // Metadata-only tracks: the probe neither scans files nor plays audio.
          auto const uri = std::format("probe/{}.flac", title);
          builder.property().uri(uri);
          auto createRes = write.tracks().create(builder, ao::library::FileManifestBuilder::makeEmpty());

          if (!createRes)
          {
            return std::unexpected{createRes.error()};
          }
        }

        return {};
      });

    if (!applyRes)
    {
      return std::unexpected{applyRes.error()};
    }

    // Close the writer and storage before the real runtime opens this library.
    return transaction.commit();
  }

  class WinUiAutomationProbe : public ApplicationT<WinUiAutomationProbe, Markup::IXamlMetadataProvider>
  {
  public:
    explicit WinUiAutomationProbe(std::int32_t& result)
      : _result{result}
    {
      UnhandledException(
        [this](Item const&, UnhandledExceptionEventArgs const& args)
        {
          fail(winrt::to_string(args.Message()));
          args.Handled(true);
          finish();
        });
      Application::LoadComponent(*this, winrt::Windows::Foundation::Uri{L"ms-appx:///App.xaml"});
    }

    void OnLaunched(LaunchActivatedEventArgs const& /*args*/)
    {
      auto resourceRes = ao::winui::configureResourceLanguage("en");
      auto catalogRes = ao::i18n::MessageCatalog::create("en");
      auto orderingRes = ao::i18n::createIcuTextOrderingPolicy("en");

      if (!resourceRes || !catalogRes || !orderingRes)
      {
        fail("English localization setup failed");
        finish();
        return;
      }

      _orderingPtr = std::move(*orderingRes);
      _aliasesPtr = ao::i18n::createIcuCompletionAliasPolicy();
      _scratch = std::filesystem::temp_directory_path() /
                 std::format("aobus-uia-audit-{}", std::chrono::steady_clock::now().time_since_epoch().count());
      auto const libraryRoot = _scratch / "library";

      if (auto seedRes = seedLibrary(libraryRoot); !seedRes)
      {
        fail(seedRes.error().message);
        finish();
        return;
      }

      _window = winrt::make<winrt::Aobus::implementation::MainWindow>();
      auto sessionRes = ao::winui::LibrarySession::create(
        _scratch,
        _window.DispatcherQueue(),
        std::move(*catalogRes),
        *_orderingPtr,
        *_aliasesPtr,
        ao::desktop::LibrarySwitchRequest{.libraryRoot = libraryRoot, .scanAfterOpen = false});

      if (!sessionRes)
      {
        fail(sessionRes.error().message);
        finish();
        return;
      }

      _sessionPtr = std::move(*sessionRes);
      ao::winui::test::addSilentAudioOutput(_sessionPtr->runtime());
      implementation()->initialize(*_sessionPtr,
                                   [](std::filesystem::path) -> ao::Result<>
                                   { return ao::makeError(ao::Error::Code::NotSupported, "Audit does not restart"); });
      _root = _window.Content().as<Grid>();
      _timeout = _window.DispatcherQueue().CreateTimer();
      _timeout.Interval(std::chrono::seconds{90});
      _timeout.IsRepeating(false);
      _timeout.Tick(
        [this](auto const&, auto const&)
        {
          fail("90-second desktop deadline expired");
          finish();
        });
      _timeout.Start();
      _loaded = _root.Loaded(winrt::auto_revoke,
                             [this](auto const&, auto const&)
                             {
                               _loaded.revoke();
                               afterFrame(&WinUiAutomationProbe::constrainWindow);
                             });
      _window.Activate();
      // Activation of SMTC/playback adapters is deliberately not requested.
    }

    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type)
    {
      return provider()->GetXamlType(type);
    }
    Markup::IXamlType GetXamlType(winrt::hstring const& name) { return provider()->GetXamlType(name); }
    winrt::com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return provider()->GetXmlnsDefinitions(); }

  private:
    using Step = void (WinUiAutomationProbe::*)();

    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> provider()
    {
      if (!_provider)
      {
        _provider = winrt::make_self<winrt::Aobus::implementation::XamlMetaDataProvider>();
      }

      return _provider;
    }

    winrt::Aobus::implementation::MainWindow* implementation()
    {
      return winrt::get_self<winrt::Aobus::implementation::MainWindow>(_window.as<winrt::Aobus::MainWindow>());
    }

    void fail(std::string_view message)
    {
      ++_failures;
      std::cerr << std::format("FAIL [{}]: {}\n", _stage, message);
    }

    void check(bool condition, std::string_view message)
    {
      if (!condition)
      {
        fail(message);
      }
    }

    void enqueue(Step next)
    {
      if (!_window.DispatcherQueue().TryEnqueue(
            [this, next]
            {
              if (_finished)
              {
                return;
              }

              try
              {
                (this->*next)();
              }
              catch (winrt::hresult_error const& error)
              {
                fail(winrt::to_string(error.message()));
                finish();
              }
              catch (std::exception const& error)
              {
                fail(error.what());
                finish();
              }
            }))
      {
        fail("dispatcher rejected audit checkpoint");
        finish();
      }
    }

    void afterFrame(Step next)
    {
      _rendering = Media::CompositionTarget::Rendering(winrt::auto_revoke,
                                                       [this, next](auto const&, auto const&)
                                                       {
                                                         _rendering.revoke();
                                                         enqueue(next);
                                                       });
    }

    Audit audit(DependencyObject const& root, std::string const& label)
    {
      auto result = Audit{};
      auditVisual(root, label, result);
      return result;
    }

    winrt::Windows::Graphics::SizeInt32 clientPixels(double const width, double const height)
    {
      auto const scale = _root.XamlRoot().RasterizationScale();
      return {
        .Width = static_cast<std::int32_t>(std::lround(width * scale)),
        .Height = static_cast<std::int32_t>(std::lround(height * scale)),
      };
    }

    static ::LRESULT CALLBACK windowSizingProcedure(::HWND handle,
                                                    ::UINT const message,
                                                    ::WPARAM const word,
                                                    ::LPARAM const parameter,
                                                    ::UINT_PTR const /*id*/,
                                                    ::DWORD_PTR const data) noexcept
    {
      auto const result = ::DefSubclassProc(handle, message, word, parameter);

      if (message == WM_GETMINMAXINFO)
      {
        // The Win32 callback ABI carries pointers in pointer-sized integers.
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto* const probe = reinterpret_cast<WinUiAutomationProbe*>(data);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto* const limits = reinterpret_cast<::MINMAXINFO*>(parameter);
        ++probe->_sizingMessages;
        // Override only this window's tracking ceiling after native handlers
        // supplied minima and maximize/work-area policy.
        limits->ptMaxTrackSize.x =
          std::max(limits->ptMinTrackSize.x, static_cast<::LONG>(probe->_maximumTrackSize.Width));
        limits->ptMaxTrackSize.y =
          std::max(limits->ptMinTrackSize.y, static_cast<::LONG>(probe->_maximumTrackSize.Height));
        return 0;
      }

      return result;
    }

    bool tryInstallWindowSizing()
    {
      winrt::check_hresult(_window.as<::IWindowNative>()->get_WindowHandle(&_windowHandle));

      if (::SetWindowSubclass(
            _windowHandle, &WinUiAutomationProbe::windowSizingProcedure, 1, reinterpret_cast<::DWORD_PTR>(this)) == 0)
      {
        fail("could not register the probe window's size-limit handler");
        return false;
      }

      _windowSizing = ao::utility::ScopedRegistration{
        [this]
        {
          check(::RemoveWindowSubclass(_windowHandle, &WinUiAutomationProbe::windowSizingProcedure, 1) != 0,
                "could not remove the probe window's size-limit handler");
        }};
      return true;
    }

    void setMaximumClientSize(winrt::Windows::Graphics::SizeInt32 const client)
    {
      auto const appWindow = _window.AppWindow();
      auto const outer = appWindow.Size();
      auto const currentClient = appWindow.ClientSize();
      _maximumTrackSize = {
        .Width = client.Width + outer.Width - currentClient.Width,
        .Height = client.Height + outer.Height - currentClient.Height,
      };
    }

    void resizeClient(winrt::Windows::Graphics::SizeInt32 const client)
    {
      auto const appWindow = _window.AppWindow();
      auto const outer = appWindow.Size();
      auto const currentClient = appWindow.ClientSize();
      check(::SetWindowPos(_windowHandle,
                           nullptr,
                           0,
                           0,
                           client.Width + outer.Width - currentClient.Width,
                           client.Height + outer.Height - currentClient.Height,
                           SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0,
            "could not resize the probe's native client area");
    }

    void resizeForLayout(double const width, double const height)
    {
      auto const client = clientPixels(width, height);
      setMaximumClientSize(client);
      resizeClient(client);
    }

    void constrainWindow()
    {
      // Cross the system ceiling even on a large local display. A merely
      // sub-wide artificial cap missed the hosted system-limit failure.
      _beyondSystemClient = clientPixels(1200, 800);
      auto const maximumWidth = ::GetSystemMetrics(SM_CXMAXTRACK);
      auto const maximumHeight = ::GetSystemMetrics(SM_CYMAXTRACK);
      check(maximumWidth > 0 && maximumHeight > 0, "could not read the default system tracking ceiling");
      _beyondSystemClient.Width = std::max(_beyondSystemClient.Width, maximumWidth + 1);
      _beyondSystemClient.Height = std::max(_beyondSystemClient.Height, maximumHeight + 1);
      _window.AppWindow().ResizeClient(_beyondSystemClient);
      afterFrame(&WinUiAutomationProbe::installWindowSizing);
    }

    void installWindowSizing()
    {
      auto const client = _window.AppWindow().ClientSize();
      std::cerr << std::format(
        "System size-limit witness: requested={}px, default client={}px\n", _beyondSystemClient.Width, client.Width);
      setMaximumClientSize(clientPixels(ao::winui::kWideShellWidth - 1.0, 800));

      if (!tryInstallWindowSizing())
      {
        finish();
        return;
      }

      resizeClient(_beyondSystemClient);
      afterFrame(&WinUiAutomationProbe::constrainedWindow);
    }

    void constrainedWindow()
    {
      _stage = "window-size constraint canary";
      auto const width = _root.ActualWidth();
      check(_sizingMessages != 0, "the native size-limit handler was never reached");
      check(width < ao::winui::kWideShellWidth,
            std::format("the imposed maximum did not constrain the wide request: {} DIP", width));
      std::cerr << std::format(
        "Window sizing canary: constrained width={} DIP; lifting past the system ceiling\n", width);
      setMaximumClientSize(_beyondSystemClient);
      resizeClient(_beyondSystemClient);
      afterFrame(&WinUiAutomationProbe::beyondSystemWindow);
    }

    void beyondSystemWindow()
    {
      auto const client = _window.AppWindow().ClientSize();
      check(client.Width == _beyondSystemClient.Width && client.Height == _beyondSystemClient.Height,
            std::format("native client remained capped: requested={}x{}px, actual={}x{}px",
                        _beyondSystemClient.Width,
                        _beyondSystemClient.Height,
                        client.Width,
                        client.Height));
      std::cerr << std::format("System size-limit witness: lifted client={}x{}px\n", client.Width, client.Height);
      resizeForLayout(1200, 800);
      afterFrame(&WinUiAutomationProbe::modern);
    }

    void requireControl(Audit const& audit,
                        AutomationControlType const role,
                        std::string_view const scope,
                        std::string_view const name = {})
    {
      check(hasControl(audit, role, scope, name),
            std::format("required on-screen UIA landmark missing: role={}, scope={}, name={}",
                        static_cast<std::int32_t>(role),
                        scope,
                        name));
    }

    void auditShell(std::string_view stage)
    {
      _stage = stage;
      auto result = audit(_root, std::string{stage});

      for (auto const& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(_root.XamlRoot()))
      {
        auditVisual(popup.Child(), std::string{stage} + "/popup", result);
      }

      auto const classic = _sessionPtr->settings().shellMode == ao::winui::ShellMode::Classic;
      auto const prefix = std::string_view{classic ? "classic" : "modern"};
      auto const table = std::format("{}-track-table", prefix);
      auto const width = _root.ActualWidth();
      check(_narrow ? width < ao::winui::kMediumShellWidth : width >= ao::winui::kWideShellWidth,
            std::format("expected {} effective layout, actual width={} DIP", _narrow ? "narrow" : "wide", width));
      requireControl(result, AutomationControlType::Slider, std::format("{}-seek", prefix));
      requireControl(result, AutomationControlType::Button, std::format("{}-soul", prefix));
      requireControl(result, AutomationControlType::Button, table, "Title");
      requireControl(result, AutomationControlType::Thumb, table, "Resize Title column");

      for (auto const* title : {"Audit Alpha", "Audit Beta"})
      {
        requireControl(result, AutomationControlType::ListItem, table, title);
      }

      if (_grouped)
      {
        requireControl(result, AutomationControlType::ListItem, table, "Audit Album");
      }

      if (!_narrow)
      {
        requireControl(result,
                       AutomationControlType::Thumb,
                       std::format("{}-navigation", prefix),
                       winrt::to_string(ao::winui::resourceHstring(L"winui_library_navigation_pane_resize")));
        requireControl(result,
                       AutomationControlType::Thumb,
                       std::format("{}-inspector", prefix),
                       winrt::to_string(ao::winui::resourceHstring(L"winui_shell_track_details_resize")));

        if (classic)
        {
          requireControl(result, AutomationControlType::TreeItem, "classic-navigation", "All Tracks");
        }
      }

      for (auto const& path : result.missing)
      {
        fail("missing UIA Name: " + path);
      }

      std::cerr << std::format(
        "UIA audit [{}]: {} named-role controls, {} missing\n", stage, result.controls.size(), result.missing.size());
    }

    static void invoke(FrameworkElement const& element)
    {
      auto const peer = FrameworkElementAutomationPeer::CreatePeerForElement(element);
      peer.GetPattern(PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
    }

    void modern()
    {
      auditShell("Modern");
      auto presentationRes = _sessionPtr->runtime().workspace().setActivePresentation("albums");
      check(static_cast<bool>(presentationRes), "could not select the real album-grouped presentation");

      if (!presentationRes)
      {
        finish();
        return;
      }

      _grouped = true;
      afterFrame(&WinUiAutomationProbe::modernGrouped);
    }

    void modernGrouped()
    {
      auditShell("Modern grouped tracks");
      _sessionPtr->runtime().notifications().post(
        ao::rt::NotificationSeverity::Info, "Audit info", ao::rt::NotificationLifetime::history());
      afterFrame(&WinUiAutomationProbe::modernInfo);
    }

    void modernInfo()
    {
      auditShell("Modern Info notification");
      _sessionPtr->runtime().notifications().post(
        ao::rt::NotificationSeverity::Warning, "Audit warning", ao::rt::NotificationLifetime::history());
      afterFrame(&WinUiAutomationProbe::modernWarning);
    }

    void modernWarning()
    {
      auditShell("Modern warning notification");
      auto const detail = findVisual<Button>(_root, L"Audit warning");
      check(static_cast<bool>(detail), "shipping warning detail action missing");

      if (!detail)
      {
        finish();
        return;
      }

      _flyout = detail.Flyout();
      _opened = _flyout.Opened(winrt::auto_revoke,
                               [this](auto const&, auto const&)
                               {
                                 _opened.revoke();
                                 afterFrame(&WinUiAutomationProbe::warningOpened);
                               });
      invoke(detail);
    }

    void warningOpened()
    {
      auditShell("Modern warning detail popup");
      _closed = _flyout.Closed(winrt::auto_revoke,
                               [this](auto const&, auto const&)
                               {
                                 _closed.revoke();
                                 afterFrame(&WinUiAutomationProbe::canarySetup);
                               });
      _flyout.Hide();
    }

    void canarySetup()
    {
      _canaries = Controls::StackPanel{};
      auto add = [this](UIElement const& element, wchar_t const* id)
      {
        AutomationProperties::SetAutomationId(element, id);
        _canaries.Children().Append(element);
      };
      add(ao::winui::makeNamed<Button>(L"Named icon"), L"named-button");
      add(ao::winui::makeNamed<Controls::Slider>(L"Named slider"), L"named-slider");
      add(ao::winui::makeNamed<Thumb>(L"Named thumb"), L"named-thumb");
      auto textButton = Button{};
      textButton.Content(winrt::box_value(L"Visible text"));
      add(textButton, L"text-content");
      auto label = Controls::TextBlock{};
      label.Text(L"External label");
      _canaries.Children().Append(label);
      auto labelled = Button{};
      AutomationProperties::SetLabeledBy(labelled, label);
      add(labelled, L"labelled-by");
      auto missing = Button{};
      missing.Content(Controls::FontIcon{});
      add(missing, L"missing-enabled");
      auto disabled = Button{};
      disabled.IsEnabled(false);
      add(disabled, L"missing-disabled");
      auto blank = Button{};
      AutomationProperties::SetName(blank, L" \t\u00a0 ");
      add(blank, L"missing-whitespace");
      auto decorative = Button{};
      AutomationProperties::SetName(decorative, L"Excluded named control");
      AutomationProperties::SetAccessibilityView(decorative, Automation::Peers::AccessibilityView::Raw);
      add(decorative, L"decorative-raw");
      _root.Children().Append(_canaries);
      afterFrame(&WinUiAutomationProbe::canaries);
    }

    void canaries()
    {
      _stage = "audit counterexamples";
      auto const result = audit(_canaries, "canaries");
      std::cerr << std::format("UIA canaries: {} expected missing-name controls detected\n", result.missing.size());
      check(result.missing.size() == 3, "expected exactly the enabled, disabled, and whitespace name violations");

      for (auto const* id : {"missing-enabled", "missing-disabled", "missing-whitespace"})
      {
        check(std::ranges::any_of(result.missing, [id](std::string const& path) { return path.contains(id); }),
              std::format("the audit did not identify {}", id));
      }

      for (auto const* id : {"named-button", "named-slider", "named-thumb", "text-content", "labelled-by"})
      {
        check(std::ranges::any_of(result.controls, [id](AuditedControl const& control) { return control.id == id; }),
              std::format("the positive {} was not audited", id));
      }

      check(!hasControl(result, AutomationControlType::Button, "decorative-raw"),
            "a named Raw-view control must not satisfy a control-view landmark");
      check(!hasControl(result, AutomationControlType::Thumb, "named-button"),
            "a matching identity with the wrong native role must not satisfy a landmark");
      check(!hasControl(result, AutomationControlType::Button, "named-thumb", "Named icon"),
            "a name outside the required scope must not satisfy a landmark");
      AutomationProperties::SetAccessibilityView(
        findVisual<Button>(_canaries, L"Named icon"), Automation::Peers::AccessibilityView::Raw);
      afterFrame(&WinUiAutomationProbe::excludedCanary);
    }

    void excludedCanary()
    {
      _stage = "landmark moved to Raw view";
      check(!hasControl(audit(_canaries, "canaries"), AutomationControlType::Button, "named-button"),
            "a previously reached named control remained a landmark after moving to Raw view");
      auto const slider = findVisual<Controls::Slider>(_canaries, L"Named slider");
      std::uint32_t index = 0;

      if (!_canaries.Children().IndexOf(slider, index))
      {
        fail("the removable slider canary was not attached");
        finish();
        return;
      }

      _canaries.Children().RemoveAt(index);
      afterFrame(&WinUiAutomationProbe::removedCanary);
    }

    void removedCanary()
    {
      _stage = "landmark removed";
      check(!hasControl(audit(_canaries, "canaries"), AutomationControlType::Slider, "named-slider"),
            "a removed named control remained an audited landmark");
      std::uint32_t index = 0;

      if (!_root.Children().IndexOf(_canaries, index))
      {
        fail("canary panel did not remain attached");
        finish();
        return;
      }

      _root.Children().RemoveAt(index);
      _canaries = nullptr;
      auto volume = findVisual<Button>(_root, L"Volume");
      check(static_cast<bool>(volume), "shipping volume button missing");

      if (!volume)
      {
        finish();
        return;
      }

      _flyout = volume.Flyout();
      _opened = _flyout.Opened(winrt::auto_revoke,
                               [this](auto const&, auto const&)
                               {
                                 _opened.revoke();
                                 afterFrame(&WinUiAutomationProbe::volumeOpened);
                               });
      invoke(volume);
    }

    void volumeOpened()
    {
      auditShell("Modern volume popup");
      auto const content = _flyout.as<Flyout>().Content().as<DependencyObject>();
      auto const popupAudit = audit(content, "volume-content");
      check(std::ranges::any_of(popupAudit.controls,
                                [](AuditedControl const& control)
                                { return control.role == AutomationControlType::Slider; }),
            "the opened shipping volume slider was actually audited");

      for (auto const& path : popupAudit.missing)
      {
        fail("missing popup UIA Name: " + path);
      }

      _closed = _flyout.Closed(winrt::auto_revoke,
                               [this](auto const&, auto const&)
                               {
                                 _closed.revoke();
                                 afterFrame(&WinUiAutomationProbe::openMore);
                               });
      _flyout.Hide();
    }

    void openMore()
    {
      auto more = findVisual<Button>(_root, L"More");
      check(static_cast<bool>(more), "shipping More button missing");

      if (!more)
      {
        finish();
        return;
      }

      _flyout = more.Flyout();
      _opened = _flyout.Opened(winrt::auto_revoke,
                               [this](auto const&, auto const&)
                               {
                                 _opened.revoke();
                                 afterFrame(&WinUiAutomationProbe::moreOpened);
                               });
      invoke(more);
    }

    void moreOpened()
    {
      auditShell("Modern menu popup");
      auto classic = Controls::MenuFlyoutItem{nullptr};

      for (auto const& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(_root.XamlRoot()))
      {
        if (auto const candidate = findVisual<Controls::MenuFlyoutItem>(popup.Child(), L"Classic Mode"); candidate)
        {
          classic = candidate;
        }
      }

      check(static_cast<bool>(classic), "shipping Classic Mode menu item missing");

      if (!classic)
      {
        finish();
        return;
      }

      invoke(classic);
      afterFrame(&WinUiAutomationProbe::classic);
    }

    void classic()
    {
      check(_sessionPtr->settings().shellMode == ao::winui::ShellMode::Classic,
            "native menu did not switch the real shell to Classic");
      auditShell("Classic");
      _narrow = true;
      resizeForLayout(640, 640);
      afterFrame(&WinUiAutomationProbe::narrowClassic);
    }

    void narrowClassic()
    {
      auditShell("Classic narrow");
      finish();
    }

    void finish()
    {
      if (std::exchange(_finished, true))
      {
        return;
      }

      _loaded.revoke();
      _rendering.revoke();
      _opened.revoke();
      _closed.revoke();

      if (_timeout)
      {
        _timeout.Stop();
      }

      _windowSizing.reset();

      if (_window)
      {
        implementation()->retire();
        _window.Close();
        _window = nullptr;
      }

      _sessionPtr.reset();
      _aliasesPtr.reset();
      _orderingPtr.reset();
      ao::winui::resetResourceLanguage();

      if (!_scratch.empty())
      {
        auto error = std::error_code{};
        std::filesystem::remove_all(_scratch, error);
        check(!error, "disposable state cleanup failed");
      }

      std::cerr << std::format("WinUI naming audit: {} failure(s)\n", _failures);
      _result = _failures == 0 ? 0 : 1;
      Exit();
    }

    std::int32_t& _result;
    std::size_t _failures = 0;
    bool _finished = false;
    bool _grouped = false;
    bool _narrow = false;
    std::string_view _stage = "startup";
    std::filesystem::path _scratch;
    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> _provider{nullptr};
    Window _window{nullptr};
    ::HWND _windowHandle = nullptr;
    winrt::Windows::Graphics::SizeInt32 _maximumTrackSize{};
    winrt::Windows::Graphics::SizeInt32 _beyondSystemClient{};
    std::size_t _sizingMessages = 0;
    ao::utility::ScopedRegistration _windowSizing;
    Grid _root{nullptr};
    Controls::StackPanel _canaries{nullptr};
    FlyoutBase _flyout{nullptr};
    std::unique_ptr<ao::rt::TextOrderingPolicy> _orderingPtr;
    std::unique_ptr<ao::rt::CompletionAliasPolicy> _aliasesPtr;
    std::unique_ptr<ao::winui::LibrarySession> _sessionPtr;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _timeout{nullptr};
    FrameworkElement::Loaded_revoker _loaded{};
    Media::CompositionTarget::Rendering_revoker _rendering{};
    FlyoutBase::Opened_revoker _opened{};
    FlyoutBase::Closed_revoker _closed{};
  };
} // namespace

int main()
{
  if (::DWORD session = 0; ::ProcessIdToSessionId(::GetCurrentProcessId(), &session) == 0 || session == 0 ||
                           session != ::WTSGetActiveConsoleSessionId() || !hasInteractiveUserToken())
  {
    std::cerr << "WinUI naming audit requires the active authenticated interactive console; no GUI was started.\n";
    return 2;
  }

  try
  {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    std::int32_t result = 1;
    Application::Start([&result](auto&&) { std::ignore = winrt::make<WinUiAutomationProbe>(result); });
    return result;
  }
  catch (winrt::hresult_error const& error)
  {
    std::cerr << winrt::to_string(error.message()) << '\n';
    return 2;
  }
  catch (std::exception const& error)
  {
    std::cerr << error.what() << '\n';
    return 2;
  }
}

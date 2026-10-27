// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Loaded-XAML observations through the shipping status.activity component,
// built by the production ComponentRegistry and its real status registration.
// The strip's elements are found by documented structure, its dismiss chrome
// carries the shipping automation name, and its label and status icon are
// checked against a live window-local foreground reference across native Light
// and Dark transitions. No physical input, audio endpoint, or user state.

#include "XamlMetaDataProvider.h"
#include "app/DispatcherQueueExecutor.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "platform/StringResources.h"
#include "theme/SurfaceBrushes.h"
#include "track/TrackListController.h"
#include <ao/Error.h>
#include <ao/async/Signal.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryDurability.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/winui/Theme.h>
#include <ao/winui/layout/LayoutSchema.h>
#include <ao/winui/layout/ShellState.h>

#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <unknwn.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
  using namespace winrt::Microsoft::UI::Xaml;
  using Item = winrt::Windows::Foundation::IInspectable;
  using Automation::AutomationProperties;
  using Controls::Button;
  using Controls::Flyout;
  using Controls::Grid;

  /// Probe-authored sentinel ink for the ancestor override observation. It is
  /// not a theme expectation: the native Light and Dark foregrounds are read
  /// from the live reference, never hardcoded here.
  constexpr auto kAncestorOverrideInk = winrt::Windows::UI::Color{.A = 255, .R = 64, .G = 200, .B = 120};

  /// The reference every foreground observation compares against: a plain
  /// window-local TextBlock that resolves the same native theme resource the
  /// shipping strip styles use.
  constexpr auto kReferenceMarkup =
    std::wstring_view{L"<TextBlock xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" "
                      L"Foreground=\"{ThemeResource ButtonForeground}\" />"};

  class WinUiActivityStatusProbeApplication
    : public ApplicationT<WinUiActivityStatusProbeApplication, Markup::IXamlMetadataProvider>
  {
  public:
    explicit WinUiActivityStatusProbeApplication(std::int32_t& exitCode)
      : _exitCode{exitCode}
    {
      UnhandledException(
        [this](Item const&, UnhandledExceptionEventArgs const& args)
        {
          record(winrt::to_string(args.Message()));
          args.Handled(true);
          finish();
        });
      Application::LoadComponent(*this, winrt::Windows::Foundation::Uri{L"ms-appx:///App.xaml"});
    }

    void OnLaunched(LaunchActivatedEventArgs const& /*args*/)
    {
      _window = Window{};
      _window.Title(L"Aobus activity status automation probe");
      _root = Grid{};
      _window.Content(_root);
      _titleBarSlot = Grid{};
      _root.Children().Append(_titleBarSlot);
      _windowClosed = _window.Closed(winrt::auto_revoke,
                                     [this](auto const&, auto const&)
                                     {
                                       if (!_finishing)
                                       {
                                         record("the window closed before the probe completed");
                                         finish();
                                       }
                                     });
      _timeout = _window.DispatcherQueue().CreateTimer();
      _timeout.Interval(std::chrono::seconds{30});
      _timeout.IsRepeating(false);
      _timeout.Tick(
        [this](auto const&, auto const&)
        {
          record("the 30-second dispatcher deadline expired");
          finish();
        });
      _timeout.Start();
      _window.Activate();
      enqueue(&WinUiActivityStatusProbeApplication::setup);
    }

    // Required IXamlMetadataProvider names belong to the platform ABI.
    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type)
    {
      return provider()->GetXamlType(type);
    }

    Markup::IXamlType GetXamlType(winrt::hstring const& name) { return provider()->GetXamlType(name); }

    winrt::com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return provider()->GetXmlnsDefinitions(); }

  private:
    using Step = void (WinUiActivityStatusProbeApplication::*)();

    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> provider()
    {
      if (!_provider)
      {
        _provider = winrt::make_self<winrt::Aobus::implementation::XamlMetaDataProvider>();
      }

      return _provider;
    }

    void record(std::string_view detail)
    {
      std::cerr << std::format("  FAIL [{}]: {}\n", _scenario, detail);
      ++_failures;
    }

    void check(bool condition, std::string_view detail)
    {
      if (!condition)
      {
        record(detail);
      }
    }

    void scenario(std::string_view name)
    {
      _scenario = name;
      std::cerr << std::format("WinUI activity status probe: scenario '{}'\n", name);
    }

    void enqueue(Step step)
    {
      if (_finishing)
      {
        return;
      }

      if (!_window.DispatcherQueue().TryEnqueue(
            [this, step]
            {
              if (_finishing)
              {
                return;
              }

              try
              {
                (this->*step)();
              }
              catch (winrt::hresult_error const& error)
              {
                record(std::format("unexpected HRESULT {:#x}: {}",
                                   static_cast<std::uint32_t>(error.code().value),
                                   winrt::to_string(error.message())));
                finish();
              }
              catch (std::exception const& error)
              {
                record(error.what());
                finish();
              }
            }))
      {
        record("the dispatcher rejected the next step");
        finish();
      }
    }

    static Automation::Provider::IInvokeProvider invokeProvider(Button const& button)
    {
      auto const peer = Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(button);
      return peer.GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>();
    }

    /// Content properties and projected interfaces can expose different ABI
    /// pointers for the same object; compare the canonical COM identity.
    static bool isSameInstance(Item const& lhs, Item const& rhs)
    {
      if (!lhs || !rhs)
      {
        return false;
      }

      return lhs.as<::IUnknown>().get() == rhs.as<::IUnknown>().get();
    }

    static winrt::hstring toolTipText(FrameworkElement const& element)
    {
      return winrt::unbox_value_or<winrt::hstring>(Controls::ToolTipService::GetToolTip(element), winrt::hstring{});
    }

    static std::string inkOf(Media::SolidColorBrush const& brush)
    {
      auto const color = brush.Color();
      return std::format("#{:02x}{:02x}{:02x}{:02x}",
                         static_cast<std::uint32_t>(color.A),
                         static_cast<std::uint32_t>(color.R),
                         static_cast<std::uint32_t>(color.G),
                         static_cast<std::uint32_t>(color.B));
    }

    // --- Setup ---------------------------------------------------------------

    void setup()
    {
      auto resourceRes = ao::winui::configureResourceLanguage("en");
      auto statusCatalogRes = ao::i18n::MessageCatalog::create("en");
      auto controllerCatalogRes = ao::i18n::MessageCatalog::create("en");

      if (!resourceRes || !statusCatalogRes || !controllerCatalogRes)
      {
        record("English catalog/resource setup failed");
        finish();
        return;
      }

      _scratch = std::filesystem::temp_directory_path() /
                 std::format("aobus-activity-probe-{}", std::chrono::steady_clock::now().time_since_epoch().count());
      std::filesystem::create_directories(_scratch);
      auto executorPtr = std::make_unique<ao::winui::DispatcherQueueExecutor>(_window.DispatcherQueue());
      auto* executorBorrow = executorPtr.get();
      auto runtimeRes = ao::rt::AppRuntime::create({
        .executorPtr = std::move(executorPtr),
        .musicRoot = _scratch,
        .databasePath = _scratch / "database",
        .musicLibraryDurability = ao::rt::LibraryDurability::Unflushed,
        .workspaceConfigStorePtr = std::make_unique<ao::rt::ConfigStore>(_scratch / "workspace.yaml"),
      });

      if (!runtimeRes)
      {
        record(runtimeRes.error().message);
        finish();
        return;
      }

      _runtimePtr = std::make_unique<ao::rt::AppRuntime>(std::move(*runtimeRes));
      _executor = executorBorrow;

      // The production status registration covers the whole status family; the
      // track controller exists only to satisfy that public registration and
      // is torn down after the strip retires.
      _columnLayoutsPtr = std::make_unique<ao::uimodel::TrackColumnLayouts>(_runtimePtr->library().changes());
      _trackListPtr = std::make_unique<ao::winui::TrackListController>(_runtimePtr->views(),
                                                                       _runtimePtr->workspace(),
                                                                       _runtimePtr->library(),
                                                                       *_columnLayoutsPtr,
                                                                       std::move(*controllerCatalogRes));

      _schemaPtr = std::make_unique<ao::uimodel::LayoutSchema>(ao::winui::layoutSchema());
      _registryPtr = std::make_unique<ao::winui::layout::ComponentRegistry>(*_schemaPtr, _actions);
      ao::winui::layout::registerStatusComponents(*_registryPtr,
                                                  _runtimePtr->views(),
                                                  _runtimePtr->notifications(),
                                                  _runtimePtr->library().jobs(),
                                                  *_trackListPtr,
                                                  std::move(*statusCatalogRes),
                                                  _shellStateChanged,
                                                  _statusMessageChanged);

      // A constructed-only donor frame owns the compiled shipping styles. It
      // never binds a session; only the two strip styles are borrowed into a
      // fresh dictionary, and the donor stays alive until the strip retires.
      _donorWindow = provider()->GetXamlType(L"Aobus.MainWindow").ActivateInstance().as<Window>();
      auto const donorResources = _donorWindow.Content().as<FrameworkElement>().Resources();
      auto const textStyleKey = winrt::box_value(L"ActivityStatusTextStyle");
      auto const iconStyleKey = winrt::box_value(L"ActivityStatusIconStyle");

      if (!donorResources.HasKey(textStyleKey) || !donorResources.HasKey(iconStyleKey))
      {
        record("the shipping frame does not carry both activity status styles");
        finish();
        return;
      }

      _donorTextStyle = donorResources.Lookup(textStyleKey).as<Style>();
      _donorIconStyle = donorResources.Lookup(iconStyleKey).as<Style>();
      // A ResourceDictionary has one owner; share the compiled styles, never
      // the donor's dictionary.
      _resources = ResourceDictionary{};
      _resources.Insert(textStyleKey, _donorTextStyle);
      _resources.Insert(iconStyleKey, _donorIconStyle);
      _root.Resources(_resources);

      _reference = Markup::XamlReader::Load(kReferenceMarkup).as<Controls::TextBlock>();
      _root.Children().Append(_reference);

      auto context = ao::winui::layout::LayoutBuildContext{
        .resources = _resources,
        .surfaceBrush = ao::winui::makeSurfaceBrushResolver(std::optional<ao::winui::Theme>{}),
        .shellState = _shellState,
        .windowActivity = _windowActivity,
        .statusMessage = _statusMessage,
        .gatePtr = {},
        .outputDeviceIntent = ao::uimodel::OutputDeviceIntent::discarded(),
        .focusedDetailPtr = {},
        .titleBarSlot = _titleBarSlot,
      };
      auto const node = ao::uimodel::LayoutNode{.id = "probe-activity-status", .type = "status.activity"};
      auto builtRes = _registryPtr->build(context, node);

      if (!builtRes)
      {
        record(std::format("status.activity build failed: {}", builtRes.error().message));
        finish();
        return;
      }

      _componentPtr = std::move(builtRes->componentPtr);

      if (!tryAdoptStripElements())
      {
        finish();
        return;
      }

      _loaded = _detail.Loaded(winrt::auto_revoke,
                               [this](auto const&, auto const&)
                               {
                                 _loaded.revoke();
                                 enqueue(&WinUiActivityStatusProbeApplication::themeLightStep);
                               });
      _root.Children().Append(_componentRoot);
    }

    /// Locate the strip's elements by documented structure, never by localized
    /// name, so a deleted or moved child fails loudly instead of passing.
    bool tryAdoptStripElements()
    {
      _componentRoot = _componentPtr->element().try_as<Grid>();

      if (!_componentRoot)
      {
        record("the shipping status.activity component does not contribute a Grid");
        return false;
      }

      if (_componentRoot.ColumnDefinitions().Size() != 2)
      {
        record("the shipping strip does not reserve its documented two columns");
        return false;
      }

      if (_componentRoot.Children().Size() != 2)
      {
        record("the shipping strip does not hold its documented detail and dismiss buttons");
        return false;
      }

      _detail = _componentRoot.Children().GetAt(0).try_as<Button>();
      _dismiss = _componentRoot.Children().GetAt(1).try_as<Button>();

      if (!_detail || !_dismiss || Grid::GetColumn(_detail) != 0 || Grid::GetColumn(_dismiss) != 1)
      {
        record("the strip's buttons are not Buttons in their documented grid columns");
        return false;
      }

      auto const content = _detail.Content().try_as<Controls::StackPanel>();

      if (!content || content.Children().Size() != 4)
      {
        record("the detail action does not hold the documented spinner, icon, label, and progress elements");
        return false;
      }

      if (!content.Children().GetAt(0).try_as<Controls::ProgressRing>() ||
          !content.Children().GetAt(3).try_as<Controls::ProgressBar>())
      {
        record("the strip's progress elements are not in their documented positions");
        return false;
      }

      _statusIcon = content.Children().GetAt(1).try_as<Controls::FontIcon>();
      _label = content.Children().GetAt(2).try_as<Controls::TextBlock>();

      if (!_statusIcon || !_label)
      {
        record("the strip's status icon and label are not in their documented positions");
        return false;
      }

      if (!_dismiss.Content().try_as<Controls::FontIcon>())
      {
        record("the compact dismiss button does not carry its glyph icon");
        return false;
      }

      check(isSameInstance(_label.Style(), _donorTextStyle),
            "the shipping strip dresses its label with the shipping text style");
      check(isSameInstance(_statusIcon.Style(), _donorIconStyle),
            "the shipping strip dresses its status icon with the shipping icon style");
      return true;
    }

    // --- Loaded-native foreground oracle --------------------------------------

    /// Wait until the requested window-local theme is observed and one native
    /// rendering pass has run. Both are events; the probe never sleeps.
    void requestTheme(ElementTheme const theme, Step const next)
    {
      if (_root.ActualTheme() == theme)
      {
        scheduleForegroundSample(next);
        return;
      }

      _actualThemeChanged = _root.ActualThemeChanged(winrt::auto_revoke,
                                                     [this, next](FrameworkElement const&, Item const&)
                                                     {
                                                       _actualThemeChanged.revoke();
                                                       scheduleForegroundSample(next);
                                                     });
      _root.RequestedTheme(theme);
    }

    void scheduleForegroundSample(Step const next)
    {
      _rendering = Media::CompositionTarget::Rendering(winrt::auto_revoke,
                                                       [this, next](Item const&, Item const&)
                                                       {
                                                         _rendering.revoke();
                                                         enqueue(next);
                                                       });
    }

    /// Compare the loaded strip's resolved label and icon foregrounds against
    /// the live reference. The oracle is the reference itself: no theme color
    /// is hardcoded, and the observed inks are recorded.
    void sampleStripForeground(winrt::Windows::UI::Color& referenceColor)
    {
      auto const referenceBrush = _reference.Foreground().try_as<Media::SolidColorBrush>();
      auto const labelBrush = _label.Foreground().try_as<Media::SolidColorBrush>();
      auto const iconBrush = _statusIcon.Foreground().try_as<Media::SolidColorBrush>();

      if (!referenceBrush || !labelBrush || !iconBrush)
      {
        record("the strip or its reference did not resolve a solid native foreground");
        return;
      }

      referenceColor = referenceBrush.Color();
      std::cerr << std::format("  observed native foregrounds: reference={} label={} icon={}\n",
                               inkOf(referenceBrush),
                               inkOf(labelBrush),
                               inkOf(iconBrush));
      check(labelBrush.Color() == referenceColor && iconBrush.Color() == referenceColor,
            "the shipping strip paints its label and status icon with the native button foreground");
      check(isSameInstance(_label.Foreground(), _reference.Foreground()) &&
              isSameInstance(_statusIcon.Foreground(), _reference.Foreground()),
            "the strip and its reference share the canonical native foreground brush");
    }

    void themeLightStep()
    {
      scenario("the loaded shipping strip follows the native window foreground");
      requestTheme(ElementTheme::Light, &WinUiActivityStatusProbeApplication::lightSampled);
    }

    void lightSampled()
    {
      check(_root.ActualTheme() == ElementTheme::Light, "the probe window adopted the requested Light theme");
      sampleStripForeground(_lightReferenceColor);
      requestTheme(ElementTheme::Dark, &WinUiActivityStatusProbeApplication::darkSampled);
    }

    void darkSampled()
    {
      check(_root.ActualTheme() == ElementTheme::Dark, "the probe window adopted the requested Dark theme");
      sampleStripForeground(_darkReferenceColor);
      check(_lightReferenceColor != _darkReferenceColor,
            "the Light and Dark window themes resolve distinct native button foregrounds");
      // The window owns the ink: a real ancestor-level ButtonForeground value
      // must reach the strip through the very same theme resolution.
      _resources.Insert(winrt::box_value(L"ButtonForeground"), Media::SolidColorBrush{kAncestorOverrideInk});
      requestTheme(ElementTheme::Light, &WinUiActivityStatusProbeApplication::overrideSampled);
    }

    void overrideSampled()
    {
      check(_root.ActualTheme() == ElementTheme::Light, "the override is observed under native Light");
      auto const referenceBrush = _reference.Foreground().try_as<Media::SolidColorBrush>();
      auto const labelBrush = _label.Foreground().try_as<Media::SolidColorBrush>();
      auto const iconBrush = _statusIcon.Foreground().try_as<Media::SolidColorBrush>();
      check(referenceBrush && referenceBrush.Color() == kAncestorOverrideInk && labelBrush &&
              labelBrush.Color() == kAncestorOverrideInk && iconBrush && iconBrush.Color() == kAncestorOverrideInk,
            "the real ancestor ButtonForeground override wins for the label, icon, and reference");
      _resources.Remove(winrt::box_value(L"ButtonForeground"));
      requestTheme(ElementTheme::Dark, &WinUiActivityStatusProbeApplication::overrideCleared);
    }

    void overrideCleared()
    {
      check(_root.ActualTheme() == ElementTheme::Dark, "the cleared override is observed under native Dark");
      auto restocked = winrt::Windows::UI::Color{};
      sampleStripForeground(restocked);
      check(restocked != kAncestorOverrideInk, "removing the ancestor override restores the native foreground");
      _root.RequestedTheme(ElementTheme::Default);
      enqueue(&WinUiActivityStatusProbeApplication::emptyIdle);
    }

    // --- Lifecycle and automation scenarios ------------------------------------

    void checkAdmission(winrt::hstring const& name, bool enabled)
    {
      check(AutomationProperties::GetName(_detail) == name, "the detail action has the expected accessible name");
      check(_detail.IsEnabled() == enabled && _detail.IsHitTestVisible() == enabled && _detail.IsTabStop() == enabled,
            "pointer, keyboard, and automation share the same admission");
      check(static_cast<bool>(_detail.Flyout()) == enabled, "a flyout is attached only for real details");
      auto const detailTip = Controls::ToolTipService::GetToolTip(_detail);
      check(enabled ? detailTip && toolTipText(_detail) == winrt::hstring{L"View activity details"} : !detailTip,
            enabled ? "an openable detail announces the shipping details tooltip"
                    : "an inactive detail carries no tooltip");
      check(_componentRoot.Visibility() == Visibility::Visible, "the reserved notification entry remains visible");
    }

    /// The compact dismiss chrome: the shipping automation peer name and its
    /// matching tooltip, both read from the real strip element.
    void checkDismissChrome()
    {
      auto const peer = Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(_dismiss);

      if (!peer)
      {
        record("the compact dismiss button has no native automation peer");
        return;
      }

      check(peer.GetName() == winrt::hstring{L"Hide notification from status"},
            "the compact dismiss automation peer carries the shipping hide name");
      check(toolTipText(_dismiss) == winrt::hstring{L"Hide notification from status"},
            "the compact dismiss tooltip matches its automation name");
    }

    void checkRejectedInvoke()
    {
      try
      {
        invokeProvider(_detail).Invoke();
        record("an empty detail action accepted automation Invoke");
      }
      catch (winrt::hresult_error const& error)
      {
        check(static_cast<std::uint32_t>(error.code().value) == UIA_E_ELEMENTNOTENABLED,
              "empty-action Invoke fails specifically with UIA_E_ELEMENTNOTENABLED");
      }

      check(!_detail.Flyout(), "rejected Invoke cannot attach an empty popup");
    }

    void emptyIdle()
    {
      scenario("empty idle is named and visible but not invokable");
      checkAdmission(L"Notifications", false);
      checkRejectedInvoke();
      _runtimePtr->notifications().post(
        ao::rt::NotificationSeverity::Info, "Probe info", ao::rt::NotificationLifetime::history());
      enqueue(&WinUiActivityStatusProbeApplication::info);
    }

    void info()
    {
      scenario("ordinary Info has no enabled empty popup");
      checkAdmission(L"Probe info", false);
      checkRejectedInvoke();
      _runtimePtr->notifications().post(
        ao::rt::NotificationSeverity::Warning, "Probe warning", ao::rt::NotificationLifetime::history());
      enqueue(&WinUiActivityStatusProbeApplication::warning);
    }

    void openDetails(Step next)
    {
      if (!_detail.Flyout())
      {
        record("an openable notification has no detail flyout");
        finish();
        return;
      }

      _flyout = _detail.Flyout().as<Flyout>();
      _opened = _flyout.Opened(winrt::auto_revoke,
                               [this, next](auto const&, auto const&)
                               {
                                 _opened.revoke();
                                 enqueue(next);
                               });
      invokeProvider(_detail).Invoke();
    }

    void warning()
    {
      scenario("warning details open through the native automation provider");
      checkAdmission(L"Probe warning", true);
      openDetails(&WinUiActivityStatusProbeApplication::warningOpened);
    }

    void checkWarningContent()
    {
      check(_flyout.IsOpen(), "automation Invoke opened the detail popup");
      auto const rows = _flyout.Content().as<Controls::ScrollViewer>().Content().as<Controls::StackPanel>();
      check(rows.Children().Size() == 1, "the popup contains one real notification row");

      if (rows.Children().Size() == 1)
      {
        std::size_t matchingMessages = 0;

        for (auto const& child : rows.Children().GetAt(0).as<Grid>().Children())
        {
          if (auto const text = child.try_as<Controls::TextBlock>(); text && text.Text() == L"Probe warning")
          {
            ++matchingMessages;
          }
        }

        check(matchingMessages == 1, "the popup renders the actual warning message");
      }
    }

    void warningOpened()
    {
      checkWarningContent();
      // Collapsed idle dismiss chrome has no peer. Observe the shipping glyph
      // only after a real notification has made it visible and loaded.
      check(_dismiss.IsLoaded() && _dismiss.Visibility() == Visibility::Visible,
            "the compact dismiss chrome is loaded and visible before its peer is queried");
      checkDismissChrome();
      auto foreground = winrt::Windows::UI::Color{};
      sampleStripForeground(foreground);
      _closed = _flyout.Closed(winrt::auto_revoke,
                               [this](auto const&, auto const&)
                               {
                                 _closed.revoke();
                                 enqueue(&WinUiActivityStatusProbeApplication::dismissCompact);
                               });
      _flyout.Hide();
    }

    void dismissCompact()
    {
      invokeProvider(_dismiss).Invoke();
      enqueue(&WinUiActivityStatusProbeApplication::retainedIdle);
    }

    void retainedIdle()
    {
      scenario("hidden compact warning remains accessible from the idle entry");
      checkAdmission(L"Notifications", true);
      check(
        _runtimePtr->notifications().feed().entries.size() == 2, "hiding compact text preserves the notification feed");
      openDetails(&WinUiActivityStatusProbeApplication::retainedOpened);
    }

    void retainedOpened()
    {
      checkWarningContent();
      scenario("hiding the last detail closes and disables the live popup");
      auto const rows = _flyout.Content().as<Controls::ScrollViewer>().Content().as<Controls::StackPanel>();
      auto dismissDetail = Button{nullptr};

      if (rows.Children().Size() == 1)
      {
        for (auto const& child : rows.Children().GetAt(0).as<Grid>().Children())
        {
          if (auto const button = child.try_as<Button>(); button)
          {
            dismissDetail = button;
          }
        }
      }

      if (!dismissDetail)
      {
        record("the history warning has no native detail-dismiss button");
        finish();
        return;
      }

      check(Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(dismissDetail).GetName() ==
              winrt::hstring{L"Hide notification from status"},
            "the detail-dismiss automation peer carries the shipping hide name");
      check(toolTipText(dismissDetail) == winrt::hstring{L"Hide notification from status"},
            "the detail-dismiss tooltip matches its automation name");
      _closed = _flyout.Closed(winrt::auto_revoke,
                               [this](auto const&, auto const&)
                               {
                                 _closed.revoke();
                                 enqueue(&WinUiActivityStatusProbeApplication::detailsCleared);
                               });
      invokeProvider(dismissDetail).Invoke();
    }

    void detailsCleared()
    {
      checkAdmission(L"Notifications", false);
      checkRejectedInvoke();
      check(!_flyout.IsOpen(), "hiding the last detail closes the existing popup");
      check(_runtimePtr->notifications().feed().entries.size() == 2,
            "hiding a detail does not remove runtime notification history");
      _runtimePtr->notifications().post(
        ao::rt::NotificationSeverity::Warning, "Probe warning", ao::rt::NotificationLifetime::history());
      enqueue(&WinUiActivityStatusProbeApplication::reopenForRetirement);
    }

    void reopenForRetirement()
    {
      scenario("new details restore the action after the last detail was hidden");
      checkAdmission(L"Probe warning", true);
      openDetails(&WinUiActivityStatusProbeApplication::retirementOpened);
    }

    void retirementOpened()
    {
      scenario("retiring the adapter closes and detaches its popup");
      checkWarningContent();
      _closed = _flyout.Closed(winrt::auto_revoke,
                               [this](auto const&, auto const&)
                               {
                                 _closed.revoke();
                                 enqueue(&WinUiActivityStatusProbeApplication::retirementClosed);
                               });
      _componentPtr.reset();
      check(!_detail.Flyout(), "teardown immediately detaches the button's flyout");
    }

    void retirementClosed()
    {
      check(!_flyout.IsOpen(), "the retained popup settles closed after adapter teardown");
      finish();
    }

    void finish()
    {
      if (std::exchange(_finishing, true))
      {
        return;
      }

      if (_timeout)
      {
        _timeout.Stop();
      }

      _loaded.revoke();
      _actualThemeChanged.revoke();
      _rendering.revoke();
      _opened.revoke();
      _closed.revoke();
      // The strip retires before every collaborator it borrows.
      _componentPtr.reset();
      _trackListPtr.reset();
      _columnLayoutsPtr.reset();

      if (_executor != nullptr)
      {
        _executor->beginClosing();
        _runtimePtr->shutdown();
        _executor->completeClosing();
      }

      _runtimePtr.reset();
      _executor = nullptr;
      _registryPtr.reset();
      _schemaPtr.reset();

      // The donor frame closes only after the strip that borrowed its styles
      // retired, and before the resource language resets.
      if (_donorWindow)
      {
        _donorWindow.Close();
        _donorWindow = nullptr;
      }

      _windowClosed.revoke();

      if (_window)
      {
        _window.Close();
      }

      _window = nullptr;
      ao::winui::resetResourceLanguage();

      if (!_scratch.empty())
      {
        auto ec = std::error_code{};
        std::filesystem::remove_all(_scratch, ec);
        check(!ec, "the disposable runtime root was removed");
      }

      _exitCode = _failures == 0 ? 0 : 1;
      std::cerr << std::format("WinUI activity status probe: {} check(s) failed\n", _failures);
      Exit();
    }

    std::int32_t& _exitCode;
    bool _finishing = false;
    std::size_t _failures = 0;
    std::string_view _scenario = "setup";
    std::filesystem::path _scratch;
    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> _provider{nullptr};
    Window _window{nullptr};
    Window _donorWindow{nullptr};
    Grid _root{nullptr};
    FrameworkElement _titleBarSlot{nullptr};
    Grid _componentRoot{nullptr};
    Button _detail{nullptr};
    Button _dismiss{nullptr};
    Controls::FontIcon _statusIcon{nullptr};
    Controls::TextBlock _label{nullptr};
    Controls::TextBlock _reference{nullptr};
    Flyout _flyout{nullptr};
    Style _donorTextStyle{nullptr};
    Style _donorIconStyle{nullptr};
    ResourceDictionary _resources{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _timeout{nullptr};
    Window::Closed_revoker _windowClosed{};
    FrameworkElement::Loaded_revoker _loaded{};
    FrameworkElement::ActualThemeChanged_revoker _actualThemeChanged{};
    Media::CompositionTarget::Rendering_revoker _rendering{};
    Controls::Primitives::FlyoutBase::Opened_revoker _opened{};
    Controls::Primitives::FlyoutBase::Closed_revoker _closed{};
    std::unique_ptr<ao::rt::AppRuntime> _runtimePtr;
    // Borrowed from the runtime after it reaches final storage.
    ao::winui::DispatcherQueueExecutor* _executor = nullptr;
    std::unique_ptr<ao::uimodel::TrackColumnLayouts> _columnLayoutsPtr;
    std::unique_ptr<ao::winui::TrackListController> _trackListPtr;
    std::unique_ptr<ao::uimodel::LayoutSchema> _schemaPtr;
    ao::winui::layout::ActionRegistry _actions;
    std::unique_ptr<ao::winui::layout::ComponentRegistry> _registryPtr;
    std::unique_ptr<ao::winui::layout::LayoutComponent> _componentPtr;
    ao::winui::ShellState _shellState{};
    ao::winui::layout::WindowActivityState _windowActivity{.visible = true, .minimized = false};
    std::string _statusMessage{};
    ao::async::Signal<ao::winui::ShellState> _shellStateChanged;
    ao::async::Signal<std::string> _statusMessageChanged;
    winrt::Windows::UI::Color _lightReferenceColor{};
    winrt::Windows::UI::Color _darkReferenceColor{};
  };
} // namespace

int main()
{
  try
  {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    std::int32_t exitCode = 1;
    Application::Start([&exitCode](auto&&)
                       { std::ignore = winrt::make<WinUiActivityStatusProbeApplication>(exitCode); });
    return exitCode;
  }
  catch (winrt::hresult_error const& error)
  {
    std::cerr << std::format("Activity status probe failed: {} ({:#x})\n",
                             winrt::to_string(error.message()),
                             static_cast<std::uint32_t>(error.code().value));
    return 2;
  }
  catch (std::exception const& error)
  {
    std::cerr << std::format("Activity status probe failed: {}\n", error.what());
    return 3;
  }
}

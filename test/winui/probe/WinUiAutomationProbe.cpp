// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Audit the loaded shipping shells: resolved UIA names, presentation picker
// history, list navigation, the selection summary, and the count-only List
// editor. Canary controls check the naming audit itself.
#include <windows.h>

// Keep the Win32 macro out of the XAML projections.
#undef GetCurrentTime

#include "MainWindow.xaml.h"
#include "WinUiSelectionEventClient.h"
#include "WinUiSilentAudio.h"
#include "WinUiTrackSelectionProbe.h"
#include "XamlMetaDataProvider.h"
#include "app/LibrarySession.h"
#include "platform/NamedControl.h"
#include "platform/ResourceLanguage.h"
#include "platform/SmtcButton.h"
#include "platform/WinRtStringResources.h"
#include "test/media/wav/TestWav.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/audio/Transport.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/i18n/IcuCompletionAliases.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListWriter.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionAliasPolicy.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/utility/Path.h>
#include <ao/utility/ScopedRegistration.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/layout/ShellState.h>

#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <commctrl.h>
#include <microsoft.ui.xaml.window.h>
#include <systemmediatransportcontrolsinterop.h>
#include <unicode/uchar.h>
#include <unicode/umachine.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
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
  using Controls::Primitives::Popup;
  using Controls::Primitives::Thumb;

  winrt::Windows::Media::SystemMediaTransportControls transportControlsForWindow(Window const& window)
  {
    using TransportControls = winrt::Windows::Media::SystemMediaTransportControls;
    ::HWND handle = nullptr;
    winrt::check_hresult(window.as<::IWindowNative>()->get_WindowHandle(&handle));
    auto const interop = winrt::get_activation_factory<TransportControls, ::ISystemMediaTransportControlsInterop>();
    auto controls = TransportControls{nullptr};
    winrt::check_hresult(interop->GetForWindow(handle, winrt::guid_of<TransportControls>(), winrt::put_abi(controls)));
    return controls;
  }

  ao::audio::OutputDeviceSelection missingOutput()
  {
    return {.backendId = ao::audio::kBackendWasapi,
            .deviceId = ao::audio::DeviceId{"unavailable-probe-device"},
            .profileId = ao::audio::kProfileShared};
  }

  constexpr auto kPrimaryOffset = std::chrono::milliseconds{250};
  // Off the five-second grid, but aligned to whole samples at any integer sample rate.
  constexpr std::chrono::milliseconds kSeekStartOffset = std::chrono::seconds{1};
  constexpr std::chrono::milliseconds kForwardOffset = std::chrono::seconds{11};
  constexpr auto kBackwardOffset = std::chrono::seconds{6};
  constexpr double kIntermediateRangeValue = 7000.0;
  constexpr std::size_t kProbeTrackCount = 12;
  constexpr std::uint32_t kBulkTrackCount = 24;
  constexpr auto kComposerFilter = R"($composer = "Bach")";
  constexpr auto kAlphaFilter = R"($title = "Audit Alpha")";
  constexpr auto kExpectedMissing = kProbeTrackCount - 1; // Only the rescan target has a physical file.
  constexpr std::size_t kRescanTrackIndex = 2;            // Neither named Bach landmark.
  constexpr std::size_t kUntitledTrackIndex = 4;
  constexpr auto kUntitledTrackUri = "probe/untitled-filename.flac";
  constexpr auto kRescanUri = "probe/selected.wav";
  constexpr auto kPreviewStatusId = L"list-editor-preview-status";
  constexpr auto kSelectionSummaryId = L"modern-selection-info";
  constexpr auto kClassicStatusId = L"classic-status-message";
  constexpr auto kSelectionDurations =
    std::array{std::chrono::seconds{123}, std::chrono::seconds{65}, std::chrono::seconds{9}, std::chrono::seconds{0}};

  struct SeededLibrary final
  {
    ao::ListId composerListId{};
    ao::ListId pinnedBachId{};
    ao::ListId previewParentId{};
    std::array<ao::TrackId, kSelectionDurations.size()> summaryTracks{};
  };

  struct ListPreviewCheck final
  {
    wchar_t const* filter;
    std::string_view status;
    bool canCreate;
  };

  // End with invalid input immediately after a nonzero result to catch stale preview text.
  constexpr auto kListPreviewChecks = std::array{
    ListPreviewCheck{.filter = L"$artist = \"No Probe Artist\"", .status = "No matches", .canCreate = true},
    ListPreviewCheck{.filter = L"$artist = \"Audit Artist\"", .status = "Matches: 12", .canCreate = true},
    ListPreviewCheck{.filter = L"$artist =", .status = "Invalid filter", .canCreate = false},
  };

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
    return std::ranges::any_of(audit.controls,
                               [=](AuditedControl const& control)
                               {
                                 return control.role == role && !control.offscreen &&
                                        (control.id == scope || std::ranges::contains(control.scopes, scope)) &&
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

  template<typename T, typename Predicate>
  T findVisualIf(DependencyObject const& root, Predicate const& matches)
  {
    if (auto const element = root.try_as<T>(); element && matches(element))
    {
      return element;
    }

    for (std::int32_t index = 0; index < Media::VisualTreeHelper::GetChildrenCount(root); ++index)
    {
      if (auto const found = findVisualIf<T>(Media::VisualTreeHelper::GetChild(root, index), matches); found)
      {
        return found;
      }
    }

    return nullptr;
  }

  template<typename T>
  T findVisual(DependencyObject const& root, winrt::hstring const& name)
  {
    return findVisualIf<T>(root,
                           [&name](T const& element)
                           {
                             auto const peer = FrameworkElementAutomationPeer::CreatePeerForElement(element);
                             return peer && peer.GetName() == name;
                           });
  }

  template<typename T>
  T findVisualById(DependencyObject const& root, winrt::hstring const& id)
  {
    return findVisualIf<T>(
      root, [&id](T const& element) { return AutomationProperties::GetAutomationId(element) == id; });
  }

  Controls::MenuFlyoutItem findMenuItem(
    winrt::Windows::Foundation::Collections::IVector<Controls::MenuFlyoutItemBase> const& items,
    wchar_t const* resourceId)
  {
    auto const label = ao::winui::resourceHstring(resourceId);

    for (auto const& entry : items)
    {
      if (auto const item = entry.try_as<Controls::MenuFlyoutItem>(); item && item.Text() == label)
      {
        return item;
      }
    }

    return nullptr;
  }

  // The opt-in playback probes copy a private PCM WAV into the disposable library.
  ao::Result<SeededLibrary> seedLibrary(std::filesystem::path const& root,
                                        std::optional<std::filesystem::path> const& optPrimaryFixture,
                                        std::size_t const trackCount = kProbeTrackCount)
  {
    if (optPrimaryFixture)
    {
      constexpr auto kMaximumFixtureBytes = std::uintmax_t{16U} * 1024U * 1024U;
      auto const& path = *optPrimaryFixture;

      if (!path.is_absolute() || path.extension() != L".wav" ||
          !std::filesystem::is_regular_file(std::filesystem::symlink_status(path)) ||
          std::filesystem::file_size(path) > kMaximumFixtureBytes)
      {
        return ao::makeError(
          ao::Error::Code::InvalidInput, "playback probes require an absolute, regular private .wav, at most 16 MiB");
      }

      std::filesystem::create_directories(root);
      std::filesystem::copy_file(path, root / "primary-0.wav");
      std::filesystem::copy_file(root / "primary-0.wav", root / "primary-1.wav");
    }

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
      [primaryPlay = optPrimaryFixture.has_value(),
       trackCount](ao::library::LibraryWrite& write) -> ao::Result<SeededLibrary>
      {
        auto seed = SeededLibrary{};

        for (std::size_t index = 0; index < trackCount; ++index)
        {
          constexpr auto kNamedTracks = std::array{"Audit Alpha", "Audit Beta"};
          auto const isNamedTrack = index < kNamedTracks.size();
          auto const title =
            isNamedTrack ? std::string{kNamedTracks.at(index)} : std::format("Preview Track {}", index);
          auto builder = ao::library::TrackBuilder::makeEmpty();
          // Keep the two existing named landmarks first in both the flat and album views.
          builder.metadata()
            .title(index == kUntitledTrackIndex ? std::string_view{} : std::string_view{title})
            .artist("Audit Artist")
            .album("Audit Album")
            .composer(isNamedTrack ? "Bach" : "Debussy")
            .year(isNamedTrack ? std::uint16_t{2001} : std::uint16_t{2000})
            .trackNumber(static_cast<std::uint16_t>(index + 1));

          // Only the Play opt-in copies the Bach files; the ordinary audit writes the rescan target later.
          auto uri = std::string{};

          if (primaryPlay && isNamedTrack)
          {
            uri = std::format("primary-{}.wav", index);
          }
          else if (index == kRescanTrackIndex)
          {
            uri = kRescanUri;
          }
          else if (index == kUntitledTrackIndex)
          {
            uri = kUntitledTrackUri;
          }
          else
          {
            uri = std::format("probe/{}.flac", title);
          }

          builder.property().uri(uri);

          if (!primaryPlay && index < kSelectionDurations.size())
          {
            builder.property().duration(kSelectionDurations.at(index));
          }

          auto createRes = write.tracks().create(builder, ao::library::FileManifestBuilder::makeEmpty());

          if (!createRes)
          {
            return std::unexpected{createRes.error()};
          }

          if (index < seed.summaryTracks.size())
          {
            seed.summaryTracks.at(index) = *createRes;
          }
        }

        auto bach = ao::library::ListBuilder::makeEmpty();
        bach.name("Bach").filter(kComposerFilter);
        bach.orderTrackIds().add(seed.summaryTracks.front()).add(seed.summaryTracks.at(1));
        auto const bachRes = write.lists().create(bach);

        if (!bachRes)
        {
          return std::unexpected{bachRes.error()};
        }

        auto const pinnedBachRes =
          write.lists().create(ao::library::ListBuilder::makeEmpty().name("Pinned Bach").filter(kComposerFilter));

        if (!pinnedBachRes)
        {
          return std::unexpected{pinnedBachRes.error()};
        }

        auto const parentRes =
          write.lists().create(ao::library::ListBuilder::makeEmpty().name("Preview parent").filter("("));

        if (!parentRes)
        {
          return std::unexpected{parentRes.error()};
        }

        seed.composerListId = *bachRes;
        seed.pinnedBachId = *pinnedBachRes;
        seed.previewParentId = *parentRes;
        return seed;
      });

    if (!applyRes)
    {
      return std::unexpected{applyRes.error()};
    }

    // Close the writer and storage before the real runtime opens this library.
    if (auto commitRes = transaction.commit(); !commitRes)
    {
      return std::unexpected{commitRes.error()};
    }

    return *applyRes;
  }

  ao::Result<std::filesystem::path> createProbeScratch(std::string_view const prefix)
  {
    auto error = std::error_code{};
    auto const parent = std::filesystem::temp_directory_path(error);

    if (error)
    {
      return ao::makeError(ao::Error::Code::IoError, error.message());
    }

    auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto scratch = parent / std::format("{}-{}-{}", prefix, ::GetCurrentProcessId(), tick);

    // An existing file or directory is never ours, even if its name matches.
    if (!std::filesystem::create_directory(scratch, error))
    {
      return ao::makeError(
        ao::Error::Code::IoError, error ? error.message() : "WinUI probe scratch directory already exists");
    }

    return scratch;
  }

  // Hand off the fixture only after successful seeding and reporting. It lives
  // across separate launches of the shipping app, unlike an in-process audit.
  std::int32_t seedAlbumColumnsFixture()
  {
    auto const scratchRes = createProbeScratch("aobus-uia-album-columns");

    if (!scratchRes)
    {
      std::cerr << scratchRes.error().message << '\n';
      return 2;
    }

    bool handedOff = false;
    auto const cleanup = ao::utility::ScopedRegistration{
      [&scratchRes, &handedOff]
      {
        if (!handedOff)
        {
          auto error = std::error_code{};
          std::filesystem::remove_all(*scratchRes, error);

          if (error)
          {
            std::cerr << std::format("Album Columns fixture cleanup failed: {}\n", error.message());
          }
        }
      }};

    if (auto const seededRes = seedLibrary(*scratchRes / "library", std::nullopt); !seededRes)
    {
      std::cerr << seededRes.error().message << '\n';
      return 2;
    }

    std::cout << ao::utility::pathToUtf8(*scratchRes) << '\n' << std::flush;
    handedOff = static_cast<bool>(std::cout);
    return handedOff ? 0 : 2;
  }

  class WinUiAutomationProbe : public ApplicationT<WinUiAutomationProbe, Markup::IXamlMetadataProvider>
  {
  public:
    WinUiAutomationProbe(std::int32_t& result,
                         std::optional<std::string_view> const optListPreviewLocale,
                         bool const classicOutput,
                         std::optional<std::filesystem::path> optPrimaryFixture,
                         bool const seekSteps,
                         std::optional<ao::winui::ShellMode> const optSeekRangeMode,
                         std::string selectionEventsRunfile)
      : _result{result}
      , _optListPreviewLocale{optListPreviewLocale}
      , _classicOutput{classicOutput}
      , _optPrimaryFixture{std::move(optPrimaryFixture)}
      , _seekSteps{seekSteps}
      , _optSeekRangeMode{optSeekRangeMode}
      , _selectionEventsRunfile{std::move(selectionEventsRunfile)}
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
      auto const locale = _optListPreviewLocale.value_or("en");
      auto resourceRes = ao::winui::configureResourceLanguage(locale);
      auto catalogRes = ao::i18n::MessageCatalog::create(locale);
      auto orderingRes = ao::i18n::createIcuTextOrderingPolicy(locale);

      if (!resourceRes || !catalogRes || !orderingRes)
      {
        fail(_optListPreviewLocale ? std::format("{} localization setup failed", locale)
                                   : "English localization setup failed");
        finish();
        return;
      }

      _orderingPtr = std::move(*orderingRes);
      _aliasesPtr = ao::i18n::createIcuCompletionAliasPolicy();
      auto scratchRes = createProbeScratch("aobus-uia-audit");

      if (!scratchRes)
      {
        fail(scratchRes.error().message);
        finish();
        return;
      }

      _scratch = std::move(*scratchRes);
      auto const libraryRoot = _scratch / "library";

      auto const selectionEvents = !_selectionEventsRunfile.empty();
      auto const seedRes =
        seedLibrary(libraryRoot, _optPrimaryFixture, selectionEvents ? kBulkTrackCount : kProbeTrackCount);

      if (!seedRes)
      {
        fail(seedRes.error().message);
        finish();
        return;
      }

      _seed = *seedRes;
      _window = winrt::make<winrt::Aobus::implementation::MainWindow>();
      auto const createSession = [&]
      {
        return ao::winui::LibrarySession::create(
          _scratch,
          _window.DispatcherQueue(),
          *catalogRes,
          *_orderingPtr,
          *_aliasesPtr,
          ao::desktop::LibrarySwitchRequest{.libraryRoot = libraryRoot, .scanAfterOpen = false});
      };

      if (!_optListPreviewLocale && !_optPrimaryFixture)
      {
        // Save an exact, flat view distinct from both the pin and the All Tracks recommendation.
        // Reopen through the shipping session so binding the frame must not replace this restored spec.
        auto priorRes = createSession();

        if (!priorRes)
        {
          fail(priorRes.error().message);
          finish();
          return;
        }

        _sessionPtr = std::move(*priorRes);
        _sessionPtr->listPresentations().setPresentationIdForList(ao::rt::kAllTracksListId, "artists");
        _sessionPtr->listPresentations().setPresentationIdForList(_seed.pinnedBachId, "technical");
        auto viewRes = _sessionPtr->runtime().workspace().navigate({
          .target = ao::rt::GlobalViewKind::AllTracks,
          .optPresentation = ao::rt::NavigationPresentation{.spec = _restoredSpec},
        });

        if (!viewRes)
        {
          fail(viewRes.error().message);
          finish();
          return;
        }

        _sessionPtr->setPreferredOutputSelection(missingOutput());

        if (_classicOutput)
        {
          _sessionPtr->settings().shellMode = ao::winui::ShellMode::Classic;
        }

        if (auto savedRes = _sessionPtr->saveSettings(); !savedRes)
        {
          fail(savedRes.error().message);
          finish();
          return;
        }

        _sessionPtr.reset();
      }

      auto sessionRes = createSession();

      if (!sessionRes)
      {
        fail(sessionRes.error().message);
        finish();
        return;
      }

      _sessionPtr = std::move(*sessionRes);

      if (_optListPreviewLocale || _optPrimaryFixture)
      {
        ao::winui::test::addSilentAudioOutput(_sessionPtr->runtime());
      }
      else
      {
        // Offer a safe choice without replacing the route the shipping session just restored.
        ao::winui::test::registerSilentAudioProvider(_sessionPtr->runtime());
        // Selection is not restored from storage. Supply a live zero-duration
        // selection before binding the shipping shell, so no event can initialize its summary.
        // The external event fixture instead starts empty.
        auto& runtime = _sessionPtr->runtime();
        _allTracksViewId = runtime.workspace().snapshot().activeViewId;

        if (!selectionEvents && !runtime.views().setSelection(_allTracksViewId, {_seed.summaryTracks.back()}))
        {
          fail("could not select the unknown-duration track before binding the shell");
          finish();
          return;
        }
      }

      if (_optSeekRangeMode)
      {
        _sessionPtr->settings().shellMode = *_optSeekRangeMode;
      }

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
          auto const status = findVisualById<Controls::TextBlock>(_root, kClassicStatusId);
          fail(status ? std::format(
                          "90-second desktop deadline expired; Classic status: '{}'", winrt::to_string(status.Text()))
                      : "90-second desktop deadline expired");
          finish();
        });
      _timeout.Start();
      _loaded = _root.Loaded(winrt::auto_revoke,
                             [this, selectionEvents](auto const&, auto const&)
                             {
                               _loaded.revoke();

                               if (selectionEvents)
                               {
                                 _window.AppWindow().ResizeClient(clientPixels(1200, 800));
                                 afterFrame(&WinUiAutomationProbe::startSelectionEvents);
                               }
                               else if (_optPrimaryFixture)
                               {
                                 _window.AppWindow().ResizeClient(clientPixels(1200, 800));
                                 afterFrame(_optSeekRangeMode ? &WinUiAutomationProbe::startSeekRange
                                                              : &WinUiAutomationProbe::startPrimaryPlay);
                               }
                               else if (_classicOutput)
                               {
                                 afterFrame(&WinUiAutomationProbe::openPendingOutputPicker);
                               }
                               else
                               {
                                 afterFrame(_optListPreviewLocale ? &WinUiAutomationProbe::sizeListPreviewWindow
                                                                  : &WinUiAutomationProbe::constrainWindow);
                               }
                             });
      _window.Activate();
      // Only the separate Classic mode activates frame-owned playback adapters (including SMTC).
      // Its private library has no audio files; that mode never issues a playback command.
      if (_classicOutput)
      {
        auto const& transport = _sessionPtr->runtime().playback().snapshot().transport;

        if (transport.output.optPendingOutputSelection != missingOutput() ||
            transport.transport != ao::audio::Transport::Idle || transport.nowPlaying.trackId != ao::kInvalidTrackId ||
            transport.occurrenceId.value != 0 || transport.hasDeferredResume)
        {
          fail("refusing native adapter activation without an idle, empty subject and held output request");
          finish();
          return;
        }

        if (auto res = implementation()->activate(); !res)
        {
          fail(res.error().message);
          finish();
        }
      }
    }

    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type)
    {
      return provider()->GetXamlType(type);
    }
    Markup::IXamlType GetXamlType(winrt::hstring const& name) { return provider()->GetXamlType(name); }
    winrt::com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return provider()->GetXmlnsDefinitions(); }

  private:
    using Step = void (WinUiAutomationProbe::*)();

    enum class SeekRangeStep : std::uint8_t
    {
      Minimum,
      Maximum,
      Interior,
    };

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
      auto const summary = findVisualById<Controls::TextBlock>(_root, kSelectionSummaryId);
      check(summary && summary.Visibility() == Visibility::Collapsed,
            "the selection summary did not yield its space to the filter below the wide tier");
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
      afterFrame(&WinUiAutomationProbe::openPendingOutputPicker);
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

    ao::rt::TrackListViewState activeState() const
    {
      auto& runtime = _sessionPtr->runtime();
      return runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    }

    // Select a realized shipping sidebar row, driving its SelectionChanged handler.
    void selectSidebarList(winrt::hstring const& name, Step next)
    {
      auto const pane = findVisualById<Controls::NavigationView>(_root, L"modern-navigation");
      auto const row = pane ? findVisual<Controls::NavigationViewItem>(pane, name) : nullptr;

      if (!row || !row.IsLoaded() || !pane.IsPaneOpen())
      {
        fail(std::format("shipping sidebar row '{}' is not available in the open pane", winrt::to_string(name)));
        finish();
        return;
      }

      pane.SelectedItem(row);
      afterFrame(next);
    }

    void checkSelectionSummary(std::string_view const scenario, std::string_view const expected)
    {
      auto const summary = findVisualById<Controls::TextBlock>(_root, kSelectionSummaryId);

      if (!summary || summary.Visibility() != Visibility::Visible)
      {
        fail(std::format("{}: shipping selection summary is missing or hidden", scenario));
        return;
      }

      auto const observed = winrt::to_string(summary.Text());
      check(observed == expected,
            std::format("{}: expected selection summary '{}', observed '{}'", scenario, expected, observed));
    }

    void checkNativeSelectionSummary()
    {
      _stage = "Modern selection summary";
      checkSelectionSummary("live unknown-duration selection at shell binding", "1 item selected");
      auto const table = findVisualById<Controls::ScrollViewer>(_root, L"modern-track-table");
      auto const rows =
        table
          ? findVisualIf<Controls::ListView>(
              table, [](auto const& list) { return list.SelectionMode() == Controls::ListViewSelectionMode::Extended; })
          : nullptr;
      auto const alpha = rows ? findVisual<Controls::ListViewItem>(rows, L"Audit Alpha") : nullptr;
      auto const beta = rows ? findVisual<Controls::ListViewItem>(rows, L"Audit Beta") : nullptr;

      auto const betaIndex = beta ? rows.IndexFromContainer(beta) : -1;

      if (!alpha || betaIndex < 0)
      {
        fail("the shipping track list did not realize both duration fixtures");
        return;
      }

      auto const play = findVisualById<Button>(_root, L"modern-soul");
      check(play && play.IsEnabled(), "the primary transport must accept the existing row selection");

      auto const checkProvider = [this, &rows](std::string_view choice, std::vector<ao::TrackId> const& ids)
      {
        if (auto const optError = ao::winui::test::trackSelectionProviderFailure(rows, ids); optError)
        {
          fail(std::format("{}: {}", choice, *optError));
        }
      };

      // Choose actual native items. Their SelectionChanged handler publishes
      // through the shipping controller; this is not physical pointer/keyboard input.
      rows.SelectedItem(rows.ItemFromContainer(alpha));
      check(activeState().selection == std::vector{_seed.summaryTracks.front()},
            "the runtime did not receive the native single-row selection");
      checkSelectionSummary("one known duration", "1 item selected (2:03)");
      checkProvider("Alpha selected", {_seed.summaryTracks.front()});
      rows.SelectRange(Data::ItemIndexRange{betaIndex, 1});
      auto const selected = activeState().selection;
      check(selected.size() == 2 && std::ranges::contains(selected, _seed.summaryTracks.front()) &&
              std::ranges::contains(selected, _seed.summaryTracks.at(1)),
            "the runtime did not receive both native rows");
      checkSelectionSummary("two known durations", "2 items selected (3:08)");
      checkProvider("Alpha and Beta selected", {_seed.summaryTracks.front(), _seed.summaryTracks.at(1)});
      rows.DeselectRange(Data::ItemIndexRange{0, rows.Items().Size()});
      check(activeState().selection.empty(), "the runtime did not receive the native clear");
      checkSelectionSummary("native clear", "");
      checkProvider("selection cleared", {});
      check(play && !play.IsEnabled(), "a ready output and visible rows alone must not enable primary Play");
      rows.SelectedItem(rows.ItemFromContainer(alpha));
      check(play && play.IsEnabled(), "a fresh native selection must re-enable primary Play");
    }

    bool tryCheckPrimary(bool condition, std::string_view message)
    {
      if (!condition)
      {
        fail(message);
        finish();
      }

      return condition;
    }

    bool tryCheckDiscardRoute()
    {
      auto const& transport = _sessionPtr->runtime().playback().snapshot().transport;
      auto const device = ao::winui::test::silentAudioDevice();
      auto const discard = ao::audio::OutputDeviceSelection{
        .backendId = device.backendId, .deviceId = device.id, .profileId = ao::audio::kProfileShared};
      return tryCheckPrimary(
        transport.ready && !transport.output.optPendingOutputSelection && transport.output.selectedDevice == discard,
        "refusing playback without the fixed, ready discard-only endpoint");
    }

    void invokePrimary(Step next)
    {
      auto const play = findVisualById<Button>(_root, L"modern-soul");

      if (tryCheckDiscardRoute() &&
          tryCheckPrimary(play && play.IsEnabled(), "the primary transport must be invokable"))
      {
        invoke(play);
        afterFrame(next);
      }
    }

    bool tryAwaitPrimaryTransport(ao::audio::Transport expected, Step retry)
    {
      if (!tryCheckDiscardRoute())
      {
        return false;
      }

      auto const observed = _sessionPtr->runtime().playback().snapshot().transport.transport;

      if (!tryCheckPrimary(observed != ao::audio::Transport::Error, "playback failed on the private WAV fixture"))
      {
        return false;
      }

      if (observed != expected)
      {
        afterFrame(retry);
        return false;
      }

      return true;
    }

    bool tryCheckSmtcPlayWithoutTarget()
    {
      _stage = "SMTC Play with no selected row";
      using SmtcButton = winrt::Windows::Media::SystemMediaTransportControlsButton;
      auto& runtime = _sessionPtr->runtime();
      auto const notificationsBefore = runtime.notifications().feed();
      ao::winui::dispatchSmtcButton(SmtcButton::Play, _sessionPtr->playbackActions());
      return tryCheckPrimary(runtime.notifications().feed().entries == notificationsBefore.entries,
                             "SMTC Play without a target must not report a launch error");
    }

    void startPrimaryPlay()
    {
      _stage = "primary Play with visible rows but no selection";
      auto& runtime = _sessionPtr->runtime();
      auto const view = runtime.workspace().snapshot().activeViewId;
      auto const play = findVisualById<Button>(_root, L"modern-soul");
      auto const projectionRes = runtime.views().findTrackListProjection(view);
      auto const& idle = runtime.playback().snapshot().transport;
      _primaryOccurrence = idle.occurrenceId;

      if (_seekSteps)
      {
        auto const slider = findVisualById<Controls::Slider>(_root, L"modern-seek");
        auto const elapsed = findVisualById<Controls::TextBlock>(_root, L"modern-elapsed");
        auto const duration = findVisualById<Controls::TextBlock>(_root, L"modern-duration");
        check(slider && !slider.IsEnabled() && elapsed && elapsed.Text() == L"00:00" && duration &&
                duration.Text() == L"00:00",
              "without a subject the seek strip must keep its idle clock template");
      }

      if (!tryCheckDiscardRoute() ||
          !tryCheckPrimary(idle.transport == ao::audio::Transport::Idle &&
                             idle.nowPlaying.trackId == ao::kInvalidTrackId && !idle.hasDeferredResume &&
                             activeState().listId == ao::rt::kAllTracksListId && projectionRes &&
                             (*projectionRes)->indexOf(_seed.summaryTracks.front()) &&
                             (*projectionRes)->indexOf(_seed.summaryTracks.at(1)) &&
                             runtime.views().setSelection(view, {}) && play && !play.IsEnabled(),
                           "an unselected, nonempty source List must disable Play") ||
          !tryCheckSmtcPlayWithoutTarget())
      {
        return;
      }

      _stage = "primary Play with visible rows but no selection";

      try
      {
        invoke(play);
      }
      catch (winrt::hresult_error const& error)
      {
        auto const code = static_cast<std::uint32_t>(error.code().value);

        if (!tryCheckPrimary(code == UIA_E_ELEMENTNOTENABLED || code == UIA_E_ELEMENTNOTAVAILABLE,
                             "unexpected error invoking disabled primary Play"))
        {
          return;
        }
      }

      afterFrame(&WinUiAutomationProbe::selectPrimarySource);
    }

    void selectPrimarySource()
    {
      auto& runtime = _sessionPtr->runtime();
      auto const view = runtime.workspace().snapshot().activeViewId;
      auto const& idle = runtime.playback().snapshot().transport;
      auto const play = findVisualById<Button>(_root, L"modern-soul");

      if (auto const trackA = _seed.summaryTracks.front();
          tryCheckPrimary(play && !play.IsEnabled() && idle.transport == ao::audio::Transport::Idle &&
                            idle.nowPlaying.trackId == ao::kInvalidTrackId && idle.occurrenceId == _primaryOccurrence &&
                            activeState().selection.empty() && runtime.views().setSelection(view, {trackA}) &&
                            runtime.views().firstVisibleSelectedTrack(view) == trackA,
                          "untargeted SMTC and disabled primary Play must leave the subject empty before selecting A"))
      {
        invokePrimary(&WinUiAutomationProbe::pausePrimaryPlay);
      }
    }

    void pausePrimaryPlay()
    {
      _stage = "fresh primary Play of A";

      if (!tryAwaitPrimaryTransport(ao::audio::Transport::Playing, &WinUiAutomationProbe::pausePrimaryPlay))
      {
        return;
      }

      auto const& transport = _sessionPtr->runtime().playback().snapshot().transport;
      _primaryOccurrence = transport.occurrenceId;

      if (tryCheckPrimary(transport.nowPlaying.trackId == _seed.summaryTracks.front() &&
                            _primaryOccurrence.value != 0 && transport.duration >= std::chrono::seconds{1},
                          "fresh Play must start A with at least one second of decodable audio"))
      {
        if (_optSeekRangeMode)
        {
          _sessionPtr->runtime().playback().commands().pause();
          afterFrame(&WinUiAutomationProbe::browseDuringPause);
        }
        else
        {
          invokePrimary(&WinUiAutomationProbe::browseDuringPause);
        }
      }
    }

    void browseDuringPause()
    {
      _stage = _optSeekRangeMode ? "pause A for seek ranges" : "pause A and navigate to the second List";

      if (!tryAwaitPrimaryTransport(ao::audio::Transport::Paused, &WinUiAutomationProbe::browseDuringPause))
      {
        return;
      }

      if (auto const offset = (_seekSteps || _optSeekRangeMode) ? kSeekStartOffset : kPrimaryOffset;
          tryCheckPrimary(_sessionPtr->runtime().playback().commands().trySeek(_primaryOccurrence, offset),
                          "could not seek paused A"))
      {
        if (_optSeekRangeMode)
        {
          afterFrame(&WinUiAutomationProbe::setSeekRangeTarget);
        }
        else if (_seekSteps)
        {
          afterFrame(&WinUiAutomationProbe::startSeekSteps);
        }
        else
        {
          selectSidebarList(L"Bach", &WinUiAutomationProbe::resumeFromOtherList);
        }
      }
    }

    void startSeekRange()
    {
      _stage = "prepare the paused seek range";

      if (!tryCheckDiscardRoute())
      {
        return;
      }

      auto& runtime = _sessionPtr->runtime();
      auto const startRes = runtime.playback().commands().startFromView(
        runtime.workspace().snapshot().activeViewId, _seed.summaryTracks.front());

      if (tryCheckPrimary(startRes.has_value(), "could not start the private seek fixture"))
      {
        afterFrame(&WinUiAutomationProbe::pausePrimaryPlay);
      }
    }

    Controls::Slider seekRangeSlider() const
    {
      auto const classic = _optSeekRangeMode == ao::winui::ShellMode::Classic;
      return findVisualById<Controls::Slider>(_root, classic ? L"classic-seek" : L"modern-seek");
    }

    void setSeekRangeTarget()
    {
      if (!tryCheckDiscardRoute())
      {
        return;
      }

      auto const& playback = _sessionPtr->runtime().playback();
      auto const transport = playback.snapshot().transport;
      auto const slider = seekRangeSlider();

      if (!tryCheckPrimary(slider && slider.IsEnabled() && transport.occurrenceId == _primaryOccurrence &&
                             transport.transport == ao::audio::Transport::Paused && transport.duration > kForwardOffset,
                           "seek range needs an enabled shipping Slider and a paused WAV longer than 11 seconds"))
      {
        return;
      }

      auto const peer = FrameworkElementAutomationPeer::CreatePeerForElement(slider);
      auto const range =
        peer ? peer.GetPattern(PatternInterface::RangeValue).try_as<Automation::Provider::IRangeValueProvider>()
             : nullptr;
      auto const elapsed = playback.elapsed();

      if (!tryCheckPrimary(range && !range.IsReadOnly() && range.Minimum() == 0.0 &&
                             range.Maximum() == static_cast<double>(transport.duration.count()) &&
                             range.Value() == static_cast<double>(elapsed.count()),
                           "the shipping range provider must expose the paused subject's position and bounds"))
      {
        return;
      }

      switch (_seekRangeStep)
      {
        case SeekRangeStep::Minimum:
          _stage = "UIA seek range minimum";
          _seekRangeElapsed = std::chrono::milliseconds::zero();
          break;
        case SeekRangeStep::Maximum:
          _stage = "UIA seek range maximum";
          _seekRangeElapsed = transport.duration;
          break;
        case SeekRangeStep::Interior:
          _stage = "fresh UIA range action after the endpoints";
          _seekRangeElapsed = kBackwardOffset;
          break;
      }

      auto const target = static_cast<double>(_seekRangeElapsed.count());
      _expectedRangeFinalRevision = transport.finalSeekRevision.value + 1;
      // Use the shipping accessibility action, not pointer routing or capture.
      range.SetValue(kIntermediateRangeValue);
      range.SetValue(target);
      check(range.Value() == target && slider.Value() == target && playback.elapsed() == elapsed &&
              playback.snapshot().transport.finalSeekRevision == transport.finalSeekRevision,
            "only the displayed target may change before the coalesced Final");
      afterFrame(&WinUiAutomationProbe::awaitSeekRangeTarget);
    }

    void awaitSeekRangeTarget()
    {
      if (!tryCheckDiscardRoute())
      {
        return;
      }

      auto const& playback = _sessionPtr->runtime().playback();
      auto const transport = playback.snapshot().transport;

      if (!tryCheckPrimary(
            transport.occurrenceId == _primaryOccurrence && transport.transport == ao::audio::Transport::Paused,
            "a range seek must preserve the paused occurrence"))
      {
        return;
      }

      if (transport.finalSeekRevision.value < _expectedRangeFinalRevision)
      {
        afterFrame(&WinUiAutomationProbe::awaitSeekRangeTarget);
        return;
      }

      auto const classic = _optSeekRangeMode == ao::winui::ShellMode::Classic;
      auto const clock = findVisualById<Controls::TextBlock>(_root, classic ? L"classic-time" : L"modern-elapsed");
      auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(_seekRangeElapsed).count();
      auto const expected = std::format("{}:{:02d}", seconds / 60, seconds % 60);

      if (auto const slider = seekRangeSlider();
          !tryCheckPrimary(transport.finalSeekRevision.value == _expectedRangeFinalRevision &&
                             playback.elapsed() == _seekRangeElapsed && slider &&
                             slider.Value() == static_cast<double>(_seekRangeElapsed.count()) && clock &&
                             (classic ? winrt::to_string(clock.Text()).starts_with(expected + " / ")
                                      : winrt::to_string(clock.Text()) == expected),
                           "one coalesced Final must seek to the target and update the shipping Slider and clock"))
      {
        return;
      }

      switch (_seekRangeStep)
      {
        case SeekRangeStep::Minimum: _seekRangeStep = SeekRangeStep::Maximum; break;
        case SeekRangeStep::Maximum: _seekRangeStep = SeekRangeStep::Interior; break;
        case SeekRangeStep::Interior: finish(); return;
      }

      setSeekRangeTarget();
    }

    // Exercise XAML's keyboard route through the probe's own focused HWND, never
    // SendInput (which could deliver to another app). Do not call the adapter or
    // set Slider.Value as a substitute for testing actual horizontal key handling.
    bool tryPostSeekArrow(::UINT const key)
    {
      ::HWND window = nullptr;
      winrt::check_hresult(_window.as<::IWindowNative>()->get_WindowHandle(&window));
      auto* const focus = ::GetFocus();

      if (!tryCheckPrimary(
            focus != nullptr && ::GetAncestor(focus, GA_ROOT) == window, "keyboard focus left the probe window"))
      {
        return false;
      }

      auto const scan = ::MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
      auto const down = static_cast<::LPARAM>(1U | (scan << 16U) | (1U << 24U));
      auto const up = down | static_cast<::LPARAM>(3U << 30U);
      return tryCheckPrimary(
        ::PostMessageW(focus, WM_KEYDOWN, key, down) != FALSE && ::PostMessageW(focus, WM_KEYUP, key, up) != FALSE,
        "could not post the probe's arrow to its focused input site");
    }

    void startSeekSteps()
    {
      _stage = "two focused Right arrows from one second";
      auto const slider = findVisualById<Controls::Slider>(_root, L"modern-seek");

      if (tryCheckPrimary(
            slider && slider.IsEnabled() && slider.Value() == static_cast<double>(kSeekStartOffset.count()) &&
              slider.Maximum() > static_cast<double>(kForwardOffset.count()) && slider.Focus(FocusState::Keyboard),
            "--seek-steps needs a focusable off-grid position and a WAV longer than 11 seconds") &&
          tryPostSeekArrow(VK_RIGHT) && tryPostSeekArrow(VK_RIGHT))
      {
        afterFrame(&WinUiAutomationProbe::awaitForwardSteps);
      }
    }

    void awaitForwardSteps()
    {
      auto const& transport = _sessionPtr->runtime().playback().snapshot().transport;

      if (transport.elapsed != kForwardOffset)
      {
        afterFrame(&WinUiAutomationProbe::awaitForwardSteps);
        return;
      }

      _stage = "focused Left from eleven seconds";

      if (tryPostSeekArrow(VK_LEFT))
      {
        afterFrame(&WinUiAutomationProbe::awaitBackwardStep);
      }
    }

    void awaitBackwardStep()
    {
      auto& runtime = _sessionPtr->runtime();
      auto const& transport = runtime.playback().snapshot().transport;

      if (transport.elapsed != kBackwardOffset)
      {
        afterFrame(&WinUiAutomationProbe::awaitBackwardStep);
        return;
      }

      if (!tryCheckPrimary(
            transport.occurrenceId == _primaryOccurrence && transport.transport == ao::audio::Transport::Paused,
            "focused seek keys must preserve the paused occurrence") ||
          !tryCheckPrimary(runtime.savePlaybackSession().has_value(), "could not save the sought offset"))
      {
        return;
      }

      // The WAV has a decoded duration, but its catalog row has none. A deferred
      // restore retains the offset without opening the decoder or deriving a total.
      if (auto const restoredRes = runtime.restorePlaybackSession();
          tryCheckPrimary(restoredRes && restoredRes->restored, "could not restore the unknown-duration cursor"))
      {
        afterFrame(&WinUiAutomationProbe::checkUnknownSeek);
      }
    }

    void checkUnknownSeek()
    {
      _stage = "restored subject with unknown duration";
      auto const& transport = _sessionPtr->runtime().playback().snapshot().transport;
      auto const slider = findVisualById<Controls::Slider>(_root, L"modern-seek");
      auto const elapsed = findVisualById<Controls::TextBlock>(_root, L"modern-elapsed");
      auto const duration = findVisualById<Controls::TextBlock>(_root, L"modern-duration");
      check(transport.nowPlaying.trackId == _seed.summaryTracks.front() && transport.occurrenceId.value != 0 &&
              transport.occurrenceId != _primaryOccurrence && transport.hasDeferredResume &&
              transport.duration == std::chrono::milliseconds{0} && transport.elapsed == kBackwardOffset,
            "restore must install a new deferred occurrence of A at six seconds without a known total");
      check(slider && !slider.IsEnabled() && !slider.Focus(FocusState::Keyboard),
            "a subject without a duration must not admit focused seek keys");
      check(elapsed && elapsed.Text() == L"0:06" && duration && duration.Text() == L"--:--",
            "an unknown total must not erase the subject's elapsed clock");
      finish();
    }

    void resumeFromOtherList()
    {
      _stage = "primary Play while browsing B";
      auto& runtime = _sessionPtr->runtime();
      auto const view = runtime.workspace().snapshot().activeViewId;
      auto const trackB = _seed.summaryTracks.at(1);
      auto const& transport = runtime.playback().snapshot().transport;

      if (tryCheckPrimary(activeState().listId == _seed.composerListId &&
                            runtime.views().setSelection(view, {trackB}) &&
                            runtime.views().firstVisibleSelectedTrack(view) == trackB &&
                            transport.transport == ao::audio::Transport::Paused &&
                            transport.occurrenceId == _primaryOccurrence && transport.elapsed == kPrimaryOffset,
                          "the second List must select B while A remains paused at the seek offset"))
      {
        invokePrimary(&WinUiAutomationProbe::primaryPlayResumed);
      }
    }

    void primaryPlayResumed()
    {
      _stage = "verify A resumed without a restart";

      if (!tryAwaitPrimaryTransport(ao::audio::Transport::Playing, &WinUiAutomationProbe::primaryPlayResumed))
      {
        return;
      }

      auto& playback = _sessionPtr->runtime().playback();
      auto const resumedOriginal = [&]
      {
        auto const& transport = playback.snapshot().transport;
        return transport.nowPlaying.trackId == _seed.summaryTracks.front() &&
               transport.occurrenceId == _primaryOccurrence && transport.elapsed == kPrimaryOffset;
      }();

      if (tryCheckPrimary(
            resumedOriginal, "Play from the second List must resume the original occurrence and exact offset") &&
          tryCheckPrimary(activeState().listId == _seed.composerListId &&
                            activeState().selection == std::vector{_seed.summaryTracks.at(1)},
                          "resuming A must leave B selected in the second List"))
      {
        playback.commands().stop();
        afterFrame(&WinUiAutomationProbe::startSmtcSelectedPlay);
      }
    }

    void startSmtcSelectedPlay()
    {
      _stage = "SMTC Play with B selected";

      if (!tryAwaitPrimaryTransport(ao::audio::Transport::Idle, &WinUiAutomationProbe::startSmtcSelectedPlay))
      {
        return;
      }

      auto& runtime = _sessionPtr->runtime();
      auto const& idle = runtime.playback().snapshot().transport;
      auto const view = runtime.workspace().snapshot().activeViewId;

      if (tryCheckPrimary(idle.nowPlaying.trackId == ao::kInvalidTrackId && !idle.hasDeferredResume &&
                            runtime.views().firstVisibleSelectedTrack(view) == _seed.summaryTracks.at(1),
                          "SMTC Play needs a visible selection of B and no subject to resume"))
      {
        // Exercise the production dispatch, not OS button delivery. SMTC has no command result reply.
        using SmtcButton = winrt::Windows::Media::SystemMediaTransportControlsButton;
        ao::winui::dispatchSmtcButton(SmtcButton::Play, _sessionPtr->playbackActions());
        afterFrame(&WinUiAutomationProbe::confirmSmtcSelectedPlay);
      }
    }

    void confirmSmtcSelectedPlay()
    {
      if (!tryAwaitPrimaryTransport(ao::audio::Transport::Playing, &WinUiAutomationProbe::confirmSmtcSelectedPlay))
      {
        return;
      }

      auto const& transport = _sessionPtr->runtime().playback().snapshot().transport;
      check(transport.nowPlaying.trackId == _seed.summaryTracks.at(1) && transport.occurrenceId.value != 0 &&
              transport.occurrenceId != _primaryOccurrence,
            "SMTC Play must start a new occurrence of the selected row B");
      finish();
    }

    void openPendingOutputPicker()
    {
      _stage = _classicOutput ? "Classic shell-owned output picker" : "Modern restored missing output";
      auto const& output = _sessionPtr->runtime().playback().snapshot().transport.output;
      check(output.optPendingOutputSelection == missingOutput(), "runtime lost the pending output request");

      check(_sessionPtr->settings().preferredOutputSelection == missingOutput(),
            "startup replaced the saved DAC preference");
      auto const picker = findVisualById<Button>(_root, _classicOutput ? L"classic-soul" : L"modern-output-device");

      if (!picker || !picker.IsEnabled())
      {
        fail("output picker unavailable when the saved endpoint is absent");
        finish();
        return;
      }

      if (!_classicOutput)
      {
        auto const tooltip = winrt::unbox_value_or<winrt::hstring>(Controls::ToolTipService::GetToolTip(picker), {});
        check(tooltip == pendingOutputStatus(), "output button tooltip did not explain the unavailable request");
      }

      if (tryCheckDiscardChoiceIdentity())
      {
        invoke(picker);
        afterFrame(&WinUiAutomationProbe::chooseSilentOutput);
      }
    }

    winrt::hstring pendingOutputStatus() const
    {
      return winrt::to_hstring(
        ao::i18n::requiredText(_sessionPtr->textCatalog(), ao::i18n::MessageId::OutputDevicePendingStatus));
    }

    bool tryCheckDiscardChoiceIdentity()
    {
      auto const silent = ao::winui::test::silentAudioDevice();
      auto const& output = _sessionPtr->runtime().playback().snapshot().transport.output;

      // The native menu is addressed by its UIA name. Check both before it
      // snapshots its rows and before invocation, never guessing by label.
      for (auto const& backend : output.availableBackends)
      {
        if (backend.id != silent.backendId &&
            std::ranges::contains(backend.devices, silent.displayName, &ao::rt::OutputDeviceSnapshot::displayName))
        {
          fail("refusing the output picker: a platform endpoint shares the discard output's name");
          finish();
          return false;
        }
      }

      return true;
    }

    void chooseSilentOutput()
    {
      if (!tryCheckDiscardChoiceIdentity())
      {
        return;
      }

      auto const silent = ao::winui::test::silentAudioDevice();

      for (auto const& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(_root.XamlRoot()))
      {
        auto const root = popup.Child();
        auto const choice = findVisual<Controls::MenuFlyoutItem>(root, winrt::to_hstring(silent.displayName));

        if (!choice || !choice.IsEnabled())
        {
          continue;
        }

        auto const status = findVisual<Controls::MenuFlyoutItem>(root, pendingOutputStatus());
        check(status && !status.IsEnabled(), "the picker must explain the unavailable DAC in a disabled item");
        auto const checked = findVisualIf<Controls::MenuFlyoutItem>(
          root, [](auto const& item) { return std::wstring_view{item.Text()}.starts_with(L"✓ "); });
        check(!checked, "the picker checked a different device while waiting for the requested endpoint");
        invoke(choice);
        afterFrame(&WinUiAutomationProbe::confirmSilentOutput);
        return;
      }

      fail("the open output picker did not offer the discard-only endpoint");
      finish();
    }

    void confirmSilentOutput()
    {
      auto const silent = ao::winui::test::silentAudioDevice();
      auto const wanted = ao::audio::OutputDeviceSelection{
        .backendId = silent.backendId, .deviceId = silent.id, .profileId = ao::audio::kProfileShared};
      auto const& output = _sessionPtr->runtime().playback().snapshot().transport.output;
      check(!output.optPendingOutputSelection && output.selectedDevice == wanted &&
              _sessionPtr->settings().preferredOutputSelection == wanted,
            "the native output choice must replace the pending request and select the discard-only endpoint");

      if (_classicOutput)
      {
        auto const& transport = _sessionPtr->runtime().playback().snapshot().transport;

        if (tryCheckDiscardRoute() &&
            tryCheckPrimary(transport.transport == ao::audio::Transport::Idle &&
                              transport.nowPlaying.trackId == ao::kInvalidTrackId &&
                              transport.occurrenceId.value == 0 && !transport.hasDeferredResume,
                            "SMTC selection readback requires an idle session without a playback subject"))
        {
          checkClassicPlayAvailability();
          finish();
        }
      }
      else
      {
        modern();
      }
    }

    void checkClassicPlayAvailability()
    {
      _stage = "Classic Play and SMTC selection-only availability";
      // Read the shipping bridge on this HWND only; do not send media commands.
      auto const controls = transportControlsForWindow(_window);
      auto& runtime = _sessionPtr->runtime();
      auto& views = runtime.views();
      auto const status = controls.PlaybackStatus();
      auto const title = controls.DisplayUpdater().MusicProperties().Title();
      std::size_t snapshotCount = 0;
      auto const snapshotSub =
        runtime.playback().events().onSnapshot([&snapshotCount](auto const&) { ++snapshotCount; });

      check(static_cast<bool>(views.setSelection(_allTracksViewId, {_seed.summaryTracks.front()})),
            "could not select a Classic track");
      auto const play = findVisualById<Button>(_root, L"classic-play-pause");
      auto const menu = findVisual<Controls::MenuBarItem>(_root, ao::winui::resourceHstring(L"shell_menu_playback"));
      auto const playItem = menu ? findMenuItem(menu.Items(), L"playback_action_play_pause") : nullptr;
      check(play && play.IsEnabled() && playItem && playItem.IsEnabled(),
            "Classic primary Play must accept the selected row");
      check(controls.IsEnabled() && controls.IsPlayEnabled(), "SMTC must offer Play for the selected row");

      check(static_cast<bool>(views.setSelection(_allTracksViewId, {})), "could not clear the Classic selection");
      check(play && !play.IsEnabled() && playItem && !playItem.IsEnabled(),
            "Classic primary Play must disable when the selection clears");
      check(!controls.IsPlayEnabled(), "SMTC must withdraw Play when only the selection clears");

      check(static_cast<bool>(views.setSelection(_allTracksViewId, {_seed.summaryTracks.front()})),
            "could not restore the Classic selection");
      check(controls.IsPlayEnabled(), "SMTC must restore Play after reselecting a row");
      check(snapshotCount == 0 && controls.PlaybackStatus() == status &&
              controls.DisplayUpdater().MusicProperties().Title() == title,
            "selection-only capability changes must not change playback status or metadata");

      implementation()->retire();
      auto const playEnabledAtRetirement = controls.IsPlayEnabled();
      // Flip the live availability after retirement, even if the platform
      // changed the flag when the session was disabled.
      if (!playEnabledAtRetirement)
      {
        check(static_cast<bool>(views.setSelection(_allTracksViewId, {})), "could not clear retired selection");
      }

      auto const selection =
        playEnabledAtRetirement ? std::vector<ao::TrackId>{} : std::vector{_seed.summaryTracks.front()};
      check(static_cast<bool>(views.setSelection(_allTracksViewId, selection)), "could not change retired selection");
      check(!controls.IsEnabled() && controls.IsPlayEnabled() == playEnabledAtRetirement,
            "retired SMTC controls must not react to later selection changes");
    }

    void modern()
    {
      auditShell("Modern restored tracks");
      auto const state = activeState();
      check(state.listId == ao::rt::kAllTracksListId && state.presentation == _restoredSpec,
            "binding the window replaced the exact restored view with a default or pin");
      check(_sessionPtr->listPresentations().presentationIdForList(state.listId) == "artists",
            "restoring the workspace rewrote the independent saved pin");
      checkNativeSelectionSummary();
      afterFrame(&WinUiAutomationProbe::selectedModern);
    }

    void startSelectionEvents()
    {
      _stage = "out-of-process UIA selection";
      auto const state = activeState();
      auto const projectionRes = _sessionPtr->runtime().views().findTrackListProjection(state.id);

      if (state.presentation.groupBy != ao::rt::TrackGroupKey::None || !projectionRes ||
          (*projectionRes)->size() != kBulkTrackCount || !state.selection.empty())
      {
        fail("expected 24 tracks in a flat, unselected shipping projection");
        finish();
        return;
      }

      ::HWND window = nullptr;
      winrt::check_hresult(_window.as<::IWindowNative>()->get_WindowHandle(&window));
      _selectionEventClientPtr =
        std::make_unique<ao::winui::test::SelectionEventClient>(_selectionEventsRunfile, _scratch, window);
      afterFrame(&WinUiAutomationProbe::pollSelectionEvents);
    }

    void pollSelectionEvents()
    {
      if (_selectionEventClientPtr->tryTakeBulkRequest())
      {
        auto const table = findVisualById<Controls::ScrollViewer>(_root, L"modern-track-table");
        auto const rows =
          table ? findVisualIf<Controls::ListView>(
                    table,
                    [](auto const& list) { return list.SelectionMode() == Controls::ListViewSelectionMode::Extended; })
                : nullptr;

        if (!rows || rows.Items().Size() != kBulkTrackCount || !activeState().selection.empty())
        {
          fail("expected the empty bulk fixture on the mounted native ListView");
          finish();
          return;
        }

        rows.SelectRange(Data::ItemIndexRange{0, kBulkTrackCount});
      }

      if (auto const optExitCode = _selectionEventClientPtr->exitCode(); optExitCode)
      {
        check(*optExitCode == 0, std::format("selection event client exited with status {}", *optExitCode));
        check(*optExitCode != 0 || activeState().selection.size() == kBulkTrackCount,
              "bulk gesture did not publish all fixture tracks");
        finish();
        return;
      }

      afterFrame(&WinUiAutomationProbe::pollSelectionEvents);
    }

    void selectedModern()
    {
      auto const summary = findVisualById<Controls::TextBlock>(_root, kSelectionSummaryId);
      check(
        summary && summary.ActualWidth() > 0 && !summary.IsTextTrimmed() && summary.Text() == L"1 item selected (2:03)",
        "the wide shell did not lay out the full selection duration");
      openListEditor();
    }

    void openPinnedComposer()
    {
      _stage = "first sidebar visit to a pinned saved List";
      auto const before = activeState();
      check(before.listId == ao::rt::kAllTracksListId && before.selection == std::vector{_seed.summaryTracks.front()},
            "closing the editor lost the All Tracks selection before navigation");
      checkSelectionSummary("before leaving All Tracks", "1 item selected (2:03)");
      auto& runtime = _sessionPtr->runtime();
      _viewsBeforePinnedList = runtime.workspace().snapshot().openViews;

      if (std::ranges::any_of(_viewsBeforePinnedList,
                              [this, &runtime](ao::rt::ViewId id)
                              { return runtime.views().trackListState(id).listId == _seed.pinnedBachId; }))
      {
        fail("Pinned Bach already has a workspace view before its first sidebar visit");
        finish();
        return;
      }

      if (_sessionPtr->listPresentations().presentationIdForList(_seed.pinnedBachId) != "technical")
      {
        fail("the Technical pin was not restored for the unopened List");
        finish();
        return;
      }

      selectSidebarList(L"Pinned Bach", &WinUiAutomationProbe::pinnedComposerOpened);
    }

    void pinnedComposerOpened()
    {
      auto const state = activeState();
      auto const* technical = ao::rt::builtinTrackPresentationPreset("technical");

      if (state.listId != _seed.pinnedBachId || !state.filterExpression.empty() ||
          std::ranges::contains(_viewsBeforePinnedList, state.id))
      {
        fail("first sidebar visit did not create a new plain view for Pinned Bach");
        finish();
        return;
      }

      check(technical != nullptr && state.presentation == technical->spec,
            "the new view did not use its saved Technical pin instead of the composer recommendation");
      checkSelectionSummary("switch from selected All Tracks to empty Pinned Bach", "");
      check(_sessionPtr->listPresentations().presentationIdForList(_seed.pinnedBachId) == "technical",
            "first sidebar visit changed the saved pin");
      selectSidebarList(L"Bach", &WinUiAutomationProbe::composerOpened);
    }

    winrt::hstring listEditorText(ao::i18n::MessageId const id) const
    {
      return winrt::to_hstring(ao::i18n::requiredText(_sessionPtr->textCatalog(), id));
    }

    void sizeListPreviewWindow()
    {
      _window.AppWindow().ResizeClient(clientPixels(1200, 800));
      afterFrame(&WinUiAutomationProbe::openListEditor);
    }

    void openListEditor()
    {
      _stage = "open New List from the sidebar";
      auto const pane = findVisualById<Controls::NavigationView>(_root, L"modern-navigation");

      if ((_optListPreviewLocale || _liveParent) && pane && !pane.IsPaneOpen())
      {
        pane.IsPaneOpen(true);
        afterFrame(&WinUiAutomationProbe::openListEditor);
        return;
      }

      auto const* const auditParentName = _liveParent ? L"Preview parent" : L"All Tracks";
      auto const parentName = winrt::hstring{_optListPreviewLocale ? L"Bach" : auditParentName};
      auto const row = pane ? findVisual<Controls::NavigationViewItem>(pane, parentName) : nullptr;
      auto const anchor = row ? row.Content().try_as<FrameworkElement>() : nullptr;
      _flyout = anchor ? anchor.ContextFlyout() : nullptr;

      if (!_flyout)
      {
        fail(std::format("shipping {} context menu is missing", winrt::to_string(parentName)));
        finish();
        return;
      }

      _opened = _flyout.Opened(winrt::auto_revoke,
                               [this](auto const&, auto const&)
                               {
                                 _opened.revoke();
                                 afterFrame(&WinUiAutomationProbe::chooseNewList);
                               });
      _flyout.ShowAt(anchor);
    }

    void chooseNewList()
    {
      auto const label = listEditorText(ao::i18n::MessageId::ListNew);

      for (auto const& item : _flyout.as<Controls::MenuFlyout>().Items())
      {
        if (auto const create = item.try_as<Controls::MenuFlyoutItem>(); create && create.Text() == label)
        {
          invoke(create);
          afterFrame(&WinUiAutomationProbe::listEditorOpened);
          return;
        }
      }

      fail("shipping New List command is missing");
      finish();
    }

    Popup listEditorPopup() const
    {
      for (auto const& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(_root.XamlRoot()))
      {
        if (auto const child = popup.Child(); child && findVisualById<Controls::TextBlock>(child, kPreviewStatusId))
        {
          return popup;
        }
      }

      return nullptr;
    }

    void listEditorOpened()
    {
      auto const editorPopup = listEditorPopup();
      auto const popup = editorPopup ? editorPopup.Child() : nullptr;

      if (!popup)
      {
        afterFrame(&WinUiAutomationProbe::listEditorOpened);
        return;
      }

      auto const inputWithHeader = [&popup](winrt::hstring const& header)
      {
        return findVisualIf<Controls::TextBox>(
          popup,
          [&header](auto const& field) { return winrt::unbox_value_or<winrt::hstring>(field.Header(), {}) == header; });
      };
      auto const name = inputWithHeader(listEditorText(ao::i18n::MessageId::ListFieldName));
      _previewFilter = inputWithHeader(listEditorText(ao::i18n::MessageId::ListFieldLocalFilter));
      _previewStatus = findVisualById<Controls::TextBlock>(popup, kPreviewStatusId);
      _previewCreate = findVisual<Button>(popup, listEditorText(ao::i18n::MessageId::CommonCreate));
      auto const cancel = findVisual<Button>(popup, listEditorText(ao::i18n::MessageId::CommonCancel));

      if (!name || !_previewFilter || !_previewCreate || !cancel || !_previewStatus.IsLoaded())
      {
        afterFrame(&WinUiAutomationProbe::listEditorOpened);
        return;
      }

      auto const initialStatus = winrt::to_string(_previewStatus.Text());

      if (_optListPreviewLocale)
      {
        _listPreviewClosed = editorPopup.Closed(winrt::auto_revoke,
                                                [this](auto const&, auto const&)
                                                { enqueue(&WinUiAutomationProbe::localizedPreviewClosed); });
        auto const expected = ao::i18n::requiredFormat(
          _sessionPtr->textCatalog(), ao::i18n::MessageId::SmartListMatchCount, {{"scope", "source"}, {"count", 2}});

        if (initialStatus != expected)
        {
          fail(std::format("expected count-only List preview '{}', observed '{}'", expected, initialStatus));
          finish();
          return;
        }

        auto options = BringIntoViewOptions{};
        options.AnimationDesired(false);
        _previewStatus.StartBringIntoView(options);
        afterFrame(&WinUiAutomationProbe::localizedPreviewReady);
        return;
      }

      if (_liveParent)
      {
        check(initialStatus == "No tracks in source", "blank child draft opens on the broken parent");
        name.Text(L"Child draft");
        afterFrame(&WinUiAutomationProbe::clickFastCreate);
        return;
      }

      check(initialStatus == "Library tracks: 12",
            std::format("expected count-only unfiltered preview 'Library tracks: 12', observed '{}'", initialStatus));
      name.Text(L"Count-only preview");
      _optPreviewStatusToken = _previewStatus.RegisterPropertyChangedCallback(
        Controls::TextBlock::TextProperty(),
        [this](auto const&, auto const&) { enqueue(&WinUiAutomationProbe::checkListPreview); });
      applyListPreviewFilter();
    }

    void applyListPreviewFilter()
    {
      auto const& scenario = kListPreviewChecks.at(_previewStep);
      _stage = scenario.status;
      _previewFilter.Text(scenario.filter);
    }

    void checkListPreview()
    {
      auto const& scenario = kListPreviewChecks.at(_previewStep);
      auto const observed = winrt::to_string(_previewStatus.Text());
      check(observed == scenario.status,
            std::format("expected count-only List preview '{}', observed '{}'", scenario.status, observed));
      check(_previewCreate.IsEnabled() == scenario.canCreate, "List editor Create has the wrong availability");

      if (++_previewStep < kListPreviewChecks.size())
      {
        applyListPreviewFilter();
        return;
      }

      cancelListEditor();
    }

    template<typename Reply>
    static ao::async::Task<void> checkMutationAsync(WinUiAutomationProbe* owner,
                                                    ao::async::Runtime* runtime,
                                                    ao::async::Task<ao::Result<Reply>> task,
                                                    Step next,
                                                    std::stop_token stopToken)
    {
      auto const result = co_await std::move(task);
      co_await runtime->resumeOnCallbackExecutorAsync(stopToken);

      if (!result && !owner->_finished)
      {
        owner->fail(result.error().message);
        owner->enqueue(&WinUiAutomationProbe::finish);
      }
      else if (next && !owner->_finished)
      {
        owner->afterFrame(next);
      }
    }

    template<typename Reply>
    void runLibraryCommand(ao::async::Task<ao::Result<Reply>> task, Step next = nullptr)
    {
      auto& runtime = _sessionPtr->runtime().async();
      runtime.spawnWithLifetime(
        _libraryTasks,
        [this, &runtime, task = std::move(task), next](std::stop_token stop) mutable
        { return checkMutationAsync(this, &runtime, std::move(task), next, stop); },
        "List probe library command");
    }

    void clickFastCreate()
    {
      if (!_previewCreate.IsEnabled())
      {
        afterFrame(&WinUiAutomationProbe::clickFastCreate);
        return;
      }

      _stage = "Create with an unsettled child filter";
      _previewRevision = _sessionPtr->runtime().library().snapshot().revision();
      _previewFilter.Text(L"$year = 2000");
      check(_previewCreate.IsEnabled() && _previewStatus.Text() == L"No tracks in source",
            "editing has not yet replaced the previous blank-draft preview");
      // Invoke the stock Button's in-process provider on the dispatcher. Its
      // Click raises PrimaryButtonClick inline; no timer or writer turn intervenes.
      invoke(_previewCreate);
      check(!_previewCreate.IsEnabled() && _previewStatus.Text() == L"Invalid filter",
            "Create synchronously rejects the current filter over a broken parent");

      // Unlike the editor, the writer accepts this locally valid predicate.
      runLibraryCommand(_sessionPtr->runtime().library().commands().previewCreateListAsync(
                          {.parentId = _seed.previewParentId, .name = "Child draft", .expression = "$year = 2000"}),
                        &WinUiAutomationProbe::repairPreviewParent);
    }

    void repairPreviewParent()
    {
      auto const snapshot = _sessionPtr->runtime().library().snapshot();
      check(snapshot.revision() == _previewRevision &&
              std::ranges::none_of(
                snapshot.lists(), [this](auto const& row) { return row.parentId == _seed.previewParentId; }),
            "rejected Create did not write a child");
      _stage = "committed parent filter updates child without a draft edit";
      _optPreviewStatusToken = _previewStatus.RegisterPropertyChangedCallback(
        Controls::TextBlock::TextProperty(),
        [this](auto const&, auto const&) { enqueue(&WinUiAutomationProbe::checkLiveParent); });
      runLibraryCommand(_sessionPtr->runtime().library().commands().updateListAsync(
        {.listId = _seed.previewParentId, .name = "Preview parent", .expression = "$composer = 'Debussy'"}));
    }

    bool hasEditorLabel(std::wstring_view expected) const
    {
      auto const popup = listEditorPopup();
      return popup && findVisualIf<Controls::TextBlock>(
                        popup.Child(), [expected](auto const& text) { return text.Text() == expected; });
    }

    void checkLiveParent()
    {
      check(listEditorPopup() && _previewCreate.IsEnabled(),
            "the unsubmitted draft remains open and becomes admissible after repair");
      check(_previewStatus.Text() == L"Matches: 10", "child count follows committed parent filter");
      check(hasEditorLabel(L"Inherited Filter: $composer = 'Debussy'"), "child shows changed direct parent");
      check(hasEditorLabel(L"Effective Filter: ($composer = 'Debussy') and ($year = 2000)"),
            "child shows changed parent combined with local filter");
      _stage = "deleted parent invalidates open child draft";
      _previewStatus.UnregisterPropertyChangedCallback(Controls::TextBlock::TextProperty(), *_optPreviewStatusToken);
      _optPreviewStatusToken = _previewStatus.RegisterPropertyChangedCallback(
        Controls::TextBlock::TextProperty(),
        [this](auto const&, auto const&) { enqueue(&WinUiAutomationProbe::checkDeletedParent); });
      runLibraryCommand(_sessionPtr->runtime().library().commands().deleteListAsync(_seed.previewParentId));
    }

    void checkDeletedParent()
    {
      check(_sessionPtr->runtime().library().snapshot().revision() == _previewRevision + 2,
            "only the external parent repair and deletion were committed");
      check(_previewStatus.Text().empty() && !_previewCreate.IsEnabled(), "missing parent is not a zero-match source");
      check(hasEditorLabel(L"Inherited Filter: This List is no longer available."),
            "missing parent is not an empty filter");
      check(hasEditorLabel(L"Effective Filter: This List is no longer available."),
            "missing parent has no effective filter");
      cancelListEditor();
    }

    void cancelListEditor()
    {
      revokeListPreviewObserver();
      auto const popup = listEditorPopup();
      auto const cancel =
        popup ? findVisual<Button>(popup.Child(), listEditorText(ao::i18n::MessageId::CommonCancel)) : nullptr;

      if (!cancel)
      {
        fail("shipping List editor Cancel is missing");
        finish();
        return;
      }

      _stage = "closing List editor";
      invoke(cancel);
      afterFrame(&WinUiAutomationProbe::listEditorClosed);
    }

    void listEditorClosed()
    {
      if (listEditorPopup())
      {
        afterFrame(&WinUiAutomationProbe::listEditorClosed);
        return;
      }

      if (!_liveParent)
      {
        _liveParent = true;
        openListEditor();
        return;
      }

      openPinnedComposer();
    }

    void localizedPreviewReady()
    {
      if (!listEditorPopup())
      {
        fail("the List editor closed before capture readiness");
        finish();
        return;
      }

      _listPreviewReady = true;
      _stage = "waiting for List preview dismissal";
      auto const observed = winrt::to_string(_previewStatus.Text());
      std::cout << std::format("LIST_PREVIEW_READY locale={} status={}\n", *_optListPreviewLocale, observed)
                << std::flush;
    }

    void localizedPreviewClosed()
    {
      check(_listPreviewReady, "the List editor was dismissed before capture readiness");
      finish();
    }

    void revokeListPreviewObserver()
    {
      if (_optPreviewStatusToken)
      {
        _previewStatus.UnregisterPropertyChangedCallback(Controls::TextBlock::TextProperty(), *_optPreviewStatusToken);
        _optPreviewStatusToken.reset();
      }

      _previewStatus = nullptr;
      _previewFilter = nullptr;
      _previewCreate = nullptr;
    }

    void composerOpened()
    {
      _stage = "unpinned composer list opened from Modern sidebar";
      auto const state = activeState();

      if (state.listId != _seed.composerListId)
      {
        fail("sidebar did not switch from Pinned Bach to the unpinned Bach List");
        finish();
        return;
      }

      _composerViewId = state.id;
      checkSelectionSummary("unpinned Bach starts with an empty selection", "");
      auto& views = _sessionPtr->runtime().views();
      check(static_cast<bool>(views.setSelection(_allTracksViewId, {_seed.summaryTracks.back()})),
            "could not select in the inactive All Tracks view");
      checkSelectionSummary("inactive selection changes do not affect Bach", "");
      check(static_cast<bool>(views.setSelection(_composerViewId, {_seed.summaryTracks.at(1)})),
            "could not select in the active Bach view");
      checkSelectionSummary("one known duration in Bach", "1 item selected (1:05)");
      checkComposerPreset("classical-composers", std::nullopt);
      auto const picker = findVisualById<Button>(_root, L"modern-presentation-button");

      if (!picker)
      {
        fail("shipping presentation button missing");
        finish();
        return;
      }

      invoke(picker);
      afterFrame(&WinUiAutomationProbe::pickTechnical);
    }

    void pickTechnical()
    {
      _stage = "Technical menu choice";
      auto const label = winrt::to_hstring(
        ao::i18n::requiredText(_sessionPtr->textCatalog(), ao::i18n::MessageId::TrackPresentationTechnical));

      for (auto const& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(_root.XamlRoot()))
      {
        if (auto const item = findVisual<Controls::MenuFlyoutItem>(popup.Child(), label); item && item.IsEnabled())
        {
          invoke(item);
          afterFrame(&WinUiAutomationProbe::pickedTechnical);
          return;
        }
      }

      fail("shipping picker did not expose an enabled Technical item");
      finish();
    }

    void checkComposerState(ao::rt::TrackPresentationSpec const& expected, std::optional<std::string_view> const optPin)
    {
      auto const state = activeState();
      check(state.id == _composerViewId && state.listId == _seed.composerListId && state.presentation == expected,
            std::format("expected exact '{}' on the original composer view, observed '{}' on view {}",
                        expected.id,
                        state.presentation.id,
                        state.id.raw()));
      check(_sessionPtr->listPresentations().presentationIdForList(_seed.composerListId) == optPin,
            std::format("expected composer pin '{}'", optPin.value_or("Auto")));
    }

    void checkComposerPreset(std::string_view id, std::optional<std::string_view> const optPin)
    {
      if (auto const* preset = ao::rt::builtinTrackPresentationPreset(id); preset != nullptr)
      {
        checkComposerState(preset->spec, optPin);
      }
      else
      {
        fail(std::format("missing builtin preset '{}'", id));
      }
    }

    void pickedTechnical()
    {
      _stage = "Technical presentation and pin after picker choice";
      checkComposerPreset("technical", "technical");

      if (auto backRes = _sessionPtr->runtime().workspace().goBack(); !backRes)
      {
        fail("Back was unavailable after the picker command");
        finish();
        return;
      }

      afterFrame(&WinUiAutomationProbe::pickerBack);
    }

    void pickerBack()
    {
      _stage = "Back after picker choice";
      checkComposerPreset("classical-composers", "technical");

      if (auto forwardRes = _sessionPtr->runtime().workspace().goForward(); !forwardRes)
      {
        fail("Forward was unavailable after replaying the previous composer presentation");
        finish();
        return;
      }

      afterFrame(&WinUiAutomationProbe::pickerForward);
    }

    void pickerForward()
    {
      _stage = "Forward after picker choice";
      checkComposerPreset("technical", "technical");
      auto const title = findVisual<Button>(_root, L"Title");

      if (!title)
      {
        fail("Technical view has no Title column header to sort");
        finish();
        return;
      }

      invoke(title);
      afterFrame(&WinUiAutomationProbe::leaveSortedComposer);
    }

    void leaveSortedComposer()
    {
      _stage = "temporary composer sort";
      _sortedSpec = activeState().presentation;
      check(_sortedSpec.id == "windows-column-sort", "the Title header did not install a temporary presentation");
      checkComposerState(_sortedSpec, "technical");
      selectSidebarList(L"All Tracks", &WinUiAutomationProbe::revisitComposer);
    }

    void revisitComposer()
    {
      _stage = "reused All Tracks with different pin";
      auto const state = activeState();
      check(state.listId == ao::rt::kAllTracksListId && state.presentation == _restoredSpec,
            "sidebar navigation did not return to the restored All Tracks presentation");
      checkSelectionSummary("switch to the existing All Tracks selection", "1 item selected");
      selectSidebarList(L"Bach", &WinUiAutomationProbe::composerReused);
    }

    void composerReused()
    {
      _stage = "pinned composer list reused after temporary sort";
      checkComposerState(_sortedSpec, "technical");
      checkSelectionSummary("switch back to the existing Bach selection", "1 item selected (1:05)");
      prepareModernRebuild();
    }

    Controls::NavigationViewItem selectedModernBach() const
    {
      auto const pane = findVisualById<Controls::NavigationView>(_root, L"modern-navigation");
      auto const selected = pane ? pane.SelectedItem().try_as<Controls::NavigationViewItem>() : nullptr;
      std::uint32_t index = 0;
      return selected && AutomationProperties::GetName(selected) == L"Bach" && pane.MenuItems().IndexOf(selected, index)
               ? selected
               : nullptr;
    }

    void prepareModernRebuild()
    {
      _stage = "Modern rebuild: filtered Bach";
      auto& runtime = _sessionPtr->runtime();
      auto const viewRes = runtime.workspace().navigate({
        .target = ao::rt::FilteredListTarget{.listId = _seed.composerListId, .filterExpression = kAlphaFilter},
        .optPresentation = ao::rt::NavigationPresentation{.spec = _sortedSpec},
      });
      _modernBachBeforeUpsert = selectedModernBach();

      if (!viewRes || activeState().id != *viewRes || !_modernBachBeforeUpsert)
      {
        fail("could not start with a filtered Bach view and native Bach sidebar selection");
        finish();
        return;
      }

      _modernFilteredViewId = *viewRes;
      // Commit outside the List editor so its own post-save navigation cannot replace the view under test.
      runLibraryCommand(runtime.library().commands().createListAsync({.name = "Rebuild witness"}),
                        &WinUiAutomationProbe::modernListRebuilt);
    }

    void modernListRebuilt()
    {
      _stage = "Modern rebuild: restored Bach selection";
      auto const bach = selectedModernBach();
      check(bach && bach != _modernBachBeforeUpsert, "rebuilt Modern sidebar did not select the replacement Bach item");
      _modernBachBeforeUpsert = nullptr;
      auto const state = activeState();
      check(state.id == _modernFilteredViewId && state.listId == _seed.composerListId &&
              state.filterExpression == kAlphaFilter && !state.optFilterError && state.presentation == _sortedSpec,
            "Modern sidebar rebuild did not keep the filtered Bach view and its exact presentation");
      selectSidebarList(L"All Tracks", &WinUiAutomationProbe::groupModern);
    }

    void groupModern()
    {
      check(activeState().listId == ao::rt::kAllTracksListId, "did not return to All Tracks for the grouped audit");
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
      auto const result = audit(_root, "Modern untitled track");
      requireControl(result, AutomationControlType::ListItem, "modern-track-table", "untitled-filename.flac");
      check(!hasControl(result, AutomationControlType::Button, "modern-track-table", "File Path"),
            "the untitled row's filename must name it even with no File Path column");
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

    void writeRescanAudio(std::chrono::seconds duration)
    {
      auto const path = _sessionPtr->runtime().musicRoot() / kRescanUri;
      std::filesystem::create_directories(path.parent_path());
      constexpr std::uint32_t kSampleRate = 8000;
      auto const samples = static_cast<std::size_t>(duration.count()) * kSampleRate;
      auto const wav = ao::test::wav::makeWav({.channels = 1,
                                               .sampleRate = kSampleRate,
                                               .bitsPerSample = 16,
                                               .audioData = std::vector<std::uint8_t>(2 * samples)});
      auto output = std::ofstream{};
      output.exceptions(std::ios::badbit | std::ios::failbit);
      output.open(path, std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<char const*>(wav.data()), static_cast<std::streamsize>(wav.size()));
      output.close();
    }

    void moreOpened()
    {
      auditShell("Modern menu popup");
      auto const rescan = findMenuItem(_flyout.as<Controls::MenuFlyout>().Items(), L"shell_scan_library");
      check(rescan && rescan.IsEnabled(), "Modern's idle Rescan menu item must be enabled");
      check(static_cast<bool>(_sessionPtr->runtime().views().setSelection(
              _allTracksViewId, {_seed.summaryTracks.at(kRescanTrackIndex)})),
            "could not establish the selected rescan target");
      checkSelectionSummary("before the committed rescan", "1 item selected (0:09)");
      writeRescanAudio(std::chrono::seconds{1});
      startRescan(&WinUiAutomationProbe::modernRescanFinished);
      check(rescan && !rescan.IsEnabled(), "Modern's busy Rescan menu item must be disabled");
    }

    void modernRescanFinished()
    {
      _stage = "Modern Rescan settled";
      checkRescanSettled();
      auto const state = activeState();
      check(state.id == _allTracksViewId && state.selection == std::vector{_seed.summaryTracks.at(kRescanTrackIndex)},
            "the rescan changed the active view or selected IDs");
      checkSelectionSummary("same selected track after rescan", "1 item selected (0:01)");
      auto const rescan = findMenuItem(_flyout.as<Controls::MenuFlyout>().Items(), L"shell_scan_library");
      check(rescan && rescan.IsEnabled(), "Modern's settled Rescan menu item must be re-enabled");
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

      // The inspector toggle is not the menu's anchor and the probe never focuses it.
      if (auto const toggle = findVisualById<Button>(_root, L"modern-inspector-toggle"); toggle && toggle.IsLoaded())
      {
        _retiredInspectorToggle = winrt::make_weak(toggle);
      }
      else
      {
        fail("could not capture the loaded Modern inspector toggle before switching shells");
      }

      _retiredSelectionSummary = findVisualById<Controls::TextBlock>(_root, kSelectionSummaryId);
      invoke(classic);
      afterFrame(&WinUiAutomationProbe::classic);
    }

    void classic()
    {
      check(_sessionPtr->settings().shellMode == ao::winui::ShellMode::Classic,
            "native menu did not switch the real shell to Classic");
      _flyout = nullptr;
      auditShell("Classic");

      if (_retiredSelectionSummary)
      {
        auto const priorText = _retiredSelectionSummary.Text();
        check(static_cast<bool>(_sessionPtr->runtime().views().setSelection(_allTracksViewId, {})),
              "could not clear selection after retiring the Modern shell");
        check(_retiredSelectionSummary.Text() == priorText, "retired Modern summary still observes selection");
        check(static_cast<bool>(_sessionPtr->runtime().views().setSelection(
                _allTracksViewId, {_seed.summaryTracks.at(kRescanTrackIndex)})),
              "could not restore the selected rescan target in Classic");
      }
      else
      {
        fail("could not retain the Modern summary for the shell handoff check");
      }

      _narrow = true;
      resizeForLayout(640, 640);
      afterFrame(&WinUiAutomationProbe::narrowClassic);
    }

    Controls::MenuFlyoutItem classicRescanMenuItem() const
    {
      auto const file = findVisual<Controls::MenuBarItem>(_root, ao::winui::resourceHstring(L"shell_menu_file"));
      return file ? findMenuItem(file.Items(), L"library_scan_action") : nullptr;
    }

    void checkClassicRescanControls(bool const enabled)
    {
      auto const toolbar = findVisual<Button>(_root, ao::winui::resourceHstring(L"library_scan_action"));
      auto const menu = classicRescanMenuItem();
      auto const open = findVisual<Button>(_root, ao::winui::resourceHstring(L"winui_library_open_label"));
      check(toolbar && toolbar.IsEnabled() == enabled, "Classic toolbar Rescan has the wrong availability");
      check(menu && menu.IsEnabled() == enabled, "Classic File > Rescan has the wrong availability");
      check(open && open.IsEnabled(), "Rescan must not disable the unrelated Open Library button");
    }

    void startRescan(Step settled, Controls::TextBlock const& classicStatus = nullptr)
    {
      _scanStarts = 0;
      auto const expected = ao::i18n::requiredFormat(
        _sessionPtr->textCatalog(), ao::i18n::MessageId::LibraryScanMissing, {{"count", kExpectedMissing}});
      auto const before = _sessionPtr->runtime().notifications().feed();
      auto const previousId = before.entries.empty() ? ao::rt::kInvalidNotificationId : before.entries.back().id;
      _rescanSub = _sessionPtr->signalRescanAvailabilityChanged().connect(
        [this, settled, previousId, expected, classicStatus]
        {
          if (_sessionPtr->canRescan())
          {
            auto const now = _sessionPtr->runtime().notifications().feed();
            check(std::ranges::any_of(now.entries,
                                      [previousId, &expected](ao::rt::NotificationEntry const& entry)
                                      {
                                        auto const* text = std::get_if<std::string>(&entry.message);
                                        return entry.id > previousId &&
                                               entry.severity == ao::rt::NotificationSeverity::Warning &&
                                               text != nullptr && *text == expected;
                                      }),
                  "Rescan became available without this scan's missing-files warning");

            if (classicStatus)
            {
              check(classicStatus.Text() == winrt::to_hstring(expected),
                    "Classic Rescan became available before its status line showed the result");
            }

            // Observe after settlement returns to the dispatcher, not inside
            // the first idle publication where a retry might still be queued.
            afterFrame(settled);
          }
          else
          {
            ++_scanStarts;
          }
        });
      _sessionPtr->rescan();
      _sessionPtr->rescan();

      if (classicStatus)
      {
        check(classicStatus.Text() == ao::winui::resourceHstring(L"winui_library_rescanning"),
              "Classic did not show the in-progress scan on its status line");
      }
    }

    void checkRescanSettled()
    {
      check(_scanStarts == 1 && _sessionPtr->canRescan(), "a second Rescan started after one busy rejection");
      _rescanSub.reset();
    }

    void narrowClassic()
    {
      auditShell("Classic narrow");
      _stage = "Classic Rescan idle";
      checkClassicRescanControls(true);
      _stage = "Classic Rescan busy";
      auto const status = findVisualById<Controls::TextBlock>(_root, kClassicStatusId);
      check(static_cast<bool>(status), "Classic status line missing");
      writeRescanAudio(std::chrono::seconds{2});
      startRescan(&WinUiAutomationProbe::classicRescanFinished, status);
      checkClassicRescanControls(false);
    }

    void classicRescanFinished()
    {
      _stage = "Classic Rescan settled";
      checkRescanSettled();
      check(_sessionPtr->runtime().views().selectionDuration(_allTracksViewId) == std::chrono::seconds{2},
            "the second rescan did not commit the replacement duration");
      check(_retiredSelectionSummary && _retiredSelectionSummary.Text() == L"1 item selected (0:01)",
            "the retired Modern summary changed after a selected-track commit");
      _retiredSelectionSummary = nullptr;
      checkClassicRescanControls(true);
      resizeForLayout(1200, 800);
      afterFrame(&WinUiAutomationProbe::restoreClassicSelection);
    }

    Controls::TreeView classicTree() const
    {
      auto const pane = findVisualById<Grid>(_root, L"classic-navigation");
      return pane ? findVisualIf<Controls::TreeView>(pane, [](auto const&) { return true; }) : nullptr;
    }

    void restoreClassicSelection()
    {
      _stage = "Classic selection restoration preserves status";
      auto const tree = classicTree();
      auto const node = tree ? tree.SelectedNode() : nullptr;

      if (!node || activeState().listId != ao::rt::kAllTracksListId)
      {
        fail("Classic tree has no active All Tracks selection to restore");
        finish();
        return;
      }

      // Force a native selection transition back to the already active List.
      tree.SelectedNode(nullptr);
      check(!tree.SelectedNode(), "Classic tree selection did not clear");
      tree.SelectedNode(node);
      check(tree.SelectedNode() == node, "Classic tree selection did not restore");
      afterFrame(&WinUiAutomationProbe::classicSelectionRestored);
    }

    void classicSelectionRestored()
    {
      auto const status = findVisualById<Controls::TextBlock>(_root, kClassicStatusId);
      auto const expected = ao::i18n::requiredFormat(
        _sessionPtr->textCatalog(), ao::i18n::MessageId::LibraryScanMissing, {{"count", kExpectedMissing}});
      check(status && status.Text() == winrt::to_hstring(expected), "selection restoration replaced the Rescan result");

      auto const tree = classicTree();
      auto const bach = classicBachNode(tree);

      if (!bach)
      {
        fail("Classic Bach row missing after selection restoration");
        finish();
        return;
      }

      tree.SelectedNode(bach);
      afterFrame(&WinUiAutomationProbe::classicNavigated);
    }

    void classicNavigated()
    {
      _stage = "Classic navigation after selection restoration";
      auto const status = findVisualById<Controls::TextBlock>(_root, kClassicStatusId);
      check(activeState().listId == _seed.composerListId, "Classic tree did not navigate to Bach");
      auto const expected =
        ao::i18n::requiredFormat(_sessionPtr->textCatalog(), ao::i18n::MessageId::WinUiListStatus, {{"list", "Bach"}});
      check(
        status && status.Text() == winrt::to_hstring(expected), "selecting a different List did not report navigation");
      prepareClassicMove();
    }

    static Controls::TreeViewNode classicBachNode(Controls::TreeView const& tree)
    {
      auto const row = tree ? findVisual<Controls::TreeViewItem>(tree, L"Bach") : nullptr;
      return row ? tree.NodeFromContainer(row) : nullptr;
    }

    Controls::ListView filteredClassicRows() const
    {
      auto const table = findVisualById<Controls::ScrollViewer>(_root, L"classic-track-table");
      auto const rows = table ? findVisualIf<Controls::ListView>(table, [](auto const&) { return true; }) : nullptr;
      auto const item = rows && rows.Items().Size() == 1 ? rows.ContainerFromIndex(0) : nullptr;
      auto const row = item ? item.try_as<Controls::ListViewItem>() : nullptr;
      auto const peer = row ? FrameworkElementAutomationPeer::CreatePeerForElement(row) : nullptr;
      return peer && peer.GetName() == L"Audit Alpha" ? rows : nullptr;
    }

    void prepareClassicMove()
    {
      _stage = "Classic Move: filtered Manual Order view";
      auto const* manual = ao::rt::builtinTrackPresentationPreset(ao::rt::kListOrderTrackPresentationId);

      if (manual == nullptr)
      {
        fail("Manual Order presentation missing");
        finish();
        return;
      }

      auto viewRes = _sessionPtr->runtime().workspace().navigate({
        .target = ao::rt::FilteredListTarget{.listId = _seed.composerListId, .filterExpression = kAlphaFilter},
        .optPresentation = ao::rt::NavigationPresentation{.spec = manual->spec},
      });

      if (!viewRes)
      {
        fail(viewRes.error().message);
        finish();
        return;
      }

      _moveViewId = *viewRes;
      afterFrame(&WinUiAutomationProbe::moveClassicSelection);
    }

    bool isClassicMoveViewIntact() const
    {
      auto const state = activeState();
      auto const* manual = ao::rt::builtinTrackPresentationPreset(ao::rt::kListOrderTrackPresentationId);
      return state.id == _moveViewId && state.listId == _seed.composerListId &&
             state.filterExpression == kAlphaFilter && !state.optFilterError && manual != nullptr &&
             state.presentation == manual->spec && state.selection == std::vector{_seed.summaryTracks.front()};
    }

    void moveClassicSelection()
    {
      auto const tree = classicTree();
      _bachBeforeMove = classicBachNode(tree);
      auto const rows = filteredClassicRows();

      if (!_bachBeforeMove || tree.SelectedNode() != _bachBeforeMove || !rows)
      {
        _stage = "Classic Move: waiting for selected Bach with only Alpha visible";
        afterFrame(&WinUiAutomationProbe::moveClassicSelection);
        return;
      }

      rows.SelectedIndex(0);
      auto& library = _sessionPtr->runtime().library();

      if (!isClassicMoveViewIntact() || library.snapshot().listOrderTrackIds(_seed.composerListId) !=
                                          std::vector{_seed.summaryTracks.front(), _seed.summaryTracks.at(1)})
      {
        fail("expected selected Alpha in filtered Manual Order and saved order Alpha, Beta before Move");
        finish();
        return;
      }

      _movedText = winrt::to_hstring(
        ao::i18n::requiredFormat(_sessionPtr->textCatalog(), ao::i18n::MessageId::ListOrderMoved, {{"count", 1}}));
      auto const status = findVisualById<Controls::TextBlock>(_root, kClassicStatusId);

      if (!status || status.Text() == _movedText)
      {
        fail("expected a Classic status line distinct from the upcoming Move result");
        finish();
        return;
      }

      // Record delivery independently of the tree rebuild. If restoration immediately
      // overwrites the text, the final assertion must read the replacement, not this sample.
      auto const token = status.RegisterPropertyChangedCallback(
        Controls::TextBlock::TextProperty(),
        [this, previous = status.Text()](DependencyObject const& sender, auto const&)
        {
          if (auto const text = sender.as<Controls::TextBlock>().Text(); text == _movedText)
          {
            _moveReported = true;
          }
          else if (text != previous)
          {
            _otherMoveStatus = text;
          }
        });
      _moveStatusObserver = ao::utility::ScopedRegistration{
        [status, token] { status.UnregisterPropertyChangedCallback(Controls::TextBlock::TextProperty(), token); }};
      _stage = "Classic Move: waiting for committed Bach upsert";
      _moveUpsertSub = library.changes().onChanged(
        [this](ao::rt::LibraryChangeSet const& change)
        { _moveUpserted = _moveUpserted || std::ranges::contains(change.listsUpserted, _seed.composerListId); });
      // Enter the same mounted action registry as Alt+End, without injecting desktop input.
      if (!implementation()->tryInvokeShellAction("track.orderMoveToBottom"))
      {
        fail("mounted shell did not dispatch Move to Bottom");
        finish();
        return;
      }

      afterFrame(&WinUiAutomationProbe::awaitClassicMoveSettlement);
    }

    bool isClassicMoveUiSettled() const
    {
      auto const tree = classicTree();
      auto const bach = classicBachNode(tree);
      auto const rows = filteredClassicRows();
      return bach && bach != _bachBeforeMove && tree.SelectedNode() == bach && rows && rows.SelectedIndex() == 0;
    }

    void awaitClassicMoveSettlement()
    {
      if (activeState().id != _moveViewId)
      {
        fail("Classic navigated away from the filtered view during Move");
        finish();
        return;
      }

      if (!_moveUpserted)
      {
        if (!_otherMoveStatus.empty())
        {
          fail(std::format("Move reported '{}' without a Bach upsert", winrt::to_string(_otherMoveStatus)));
          finish();
          return;
        }

        afterFrame(&WinUiAutomationProbe::awaitClassicMoveSettlement);
        return;
      }

      _moveUpsertSub.reset();
      _stage = _moveReported ? "Classic Move: waiting for rebuilt Bach and selected Alpha"
                             : "Classic Move: waiting for the command result after upsert";

      if (!_moveReported || !isClassicMoveUiSettled())
      {
        afterFrame(&WinUiAutomationProbe::awaitClassicMoveSettlement);
        return;
      }

      // Do not pass on the first sight of the outcome. Let delayed tree events run.
      afterFrame(&WinUiAutomationProbe::classicMoveSettled);
    }

    void classicMoveSettled()
    {
      _stage = "Classic Move settled";
      {
        auto const snapshot = _sessionPtr->runtime().library().snapshot();
        auto const optBach = snapshot.listNode(_seed.composerListId);
        check(optBach && optBach->expression == kComposerFilter, "Move changed the saved Bach filter");
        check(snapshot.listOrderTrackIds(_seed.composerListId) ==
                std::vector{_seed.summaryTracks.at(1), _seed.summaryTracks.front()},
              "Move to Bottom did not save Beta, Alpha across the view filter");
      }
      check(isClassicMoveViewIntact(), "Move lost the exact filtered view, presentation or track selection");
      check(isClassicMoveUiSettled(), "rebuilt Classic tree or track row lost its selection");
      auto const status = findVisualById<Controls::TextBlock>(_root, kClassicStatusId);
      check(
        status && status.Visibility() == Visibility::Visible && status.ActualWidth() > 0 && status.Text() == _movedText,
        "Classic status line did not retain the Move outcome after tree restoration");

      // Check after the existing Classic frames, rescan and navigation have let
      // XAML release the retired tree and settle popup dismissal and focus.
      _stage = "retired Modern shell release";
      check(!_retiredInspectorToggle.get(), "the retired Modern inspector toggle still has a native owner");
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
      _listPreviewClosed.revoke();
      revokeListPreviewObserver();

      if (_timeout)
      {
        _timeout.Stop();
      }

      if (_selectionEventClientPtr)
      {
        _selectionEventClientPtr->log();
        _selectionEventClientPtr.reset();
      }

      _windowSizing.reset();
      _rescanSub.reset();
      _libraryTasks.cancelAll();
      _modernBachBeforeUpsert = nullptr;
      _moveUpsertSub.reset();
      _moveStatusObserver.reset();
      _bachBeforeMove = nullptr;

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

      auto label = std::string_view{_optListPreviewLocale ? "localized List preview" : "naming audit"};

      if (!_selectionEventsRunfile.empty())
      {
        label = "selection event probe";
      }

      if (_optSeekRangeMode)
      {
        label = "seek range probe";
      }

      std::cerr << std::format("WinUI {}: {} failure(s)\n", label, _failures);
      _result = _failures == 0 ? 0 : 1;
      Exit();
    }

    std::int32_t& _result;
    std::optional<std::string_view> const _optListPreviewLocale;
    bool const _classicOutput;
    std::optional<std::filesystem::path> const _optPrimaryFixture;
    bool const _seekSteps;
    std::optional<ao::winui::ShellMode> const _optSeekRangeMode;
    std::string const _selectionEventsRunfile;
    std::unique_ptr<ao::winui::test::SelectionEventClient> _selectionEventClientPtr;
    ao::rt::PlaybackOccurrenceId _primaryOccurrence{};
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
    Controls::TextBox _previewFilter{nullptr};
    Controls::TextBlock _previewStatus{nullptr};
    Button _previewCreate{nullptr};
    std::optional<std::int64_t> _optPreviewStatusToken;
    std::size_t _previewStep = 0;
    bool _listPreviewReady = false;
    bool _liveParent = false;
    std::uint64_t _previewRevision = 0;
    ao::async::LifetimeScope _libraryTasks;
    Popup::Closed_revoker _listPreviewClosed{};
    Controls::StackPanel _canaries{nullptr};
    FlyoutBase _flyout{nullptr};
    std::unique_ptr<ao::rt::TextOrderingPolicy> _orderingPtr;
    std::unique_ptr<ao::rt::CompletionAliasPolicy> _aliasesPtr;
    std::unique_ptr<ao::winui::LibrarySession> _sessionPtr;
    std::uint64_t _expectedRangeFinalRevision = 0;
    std::chrono::milliseconds _seekRangeElapsed{};
    SeekRangeStep _seekRangeStep = SeekRangeStep::Minimum;
    ao::rt::ViewId _modernFilteredViewId{};
    Controls::NavigationViewItem _modernBachBeforeUpsert{nullptr};
    ao::rt::ViewId _moveViewId{};
    Controls::TreeViewNode _bachBeforeMove{nullptr};
    ao::async::Subscription _moveUpsertSub;
    winrt::hstring _movedText;
    winrt::hstring _otherMoveStatus;
    ao::utility::ScopedRegistration _moveStatusObserver;
    bool _moveReported = false;
    bool _moveUpserted = false;
    ao::async::Subscription _rescanSub;
    std::size_t _scanStarts = 0;
    SeededLibrary _seed;
    std::vector<ao::rt::ViewId> _viewsBeforePinnedList;
    ao::rt::ViewId _allTracksViewId{};
    ao::rt::ViewId _composerViewId{};
    Controls::TextBlock _retiredSelectionSummary{nullptr};
    winrt::weak_ref<Button> _retiredInspectorToggle;
    ao::rt::TrackPresentationSpec const _restoredSpec{
      .id = "windows-column-sort",
      .sortBy = {{.field = ao::rt::TrackSortField::Year, .ascending = false}},
      .visibleFields = {ao::rt::TrackField::Title, ao::rt::TrackField::Year},
    };
    ao::rt::TrackPresentationSpec _sortedSpec;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _timeout{nullptr};
    FrameworkElement::Loaded_revoker _loaded{};
    Media::CompositionTarget::Rendering_revoker _rendering{};
    FlyoutBase::Opened_revoker _opened{};
    FlyoutBase::Closed_revoker _closed{};
  };
} // namespace

int main(int argc, char** argv)
{
  auto optListPreviewLocale = std::optional<std::string_view>{};

  auto const seedAlbumColumns = argc == 2 && std::string_view{argv[1]} == "--seed-album-columns";
  auto const classicOutput = argc == 2 && std::string_view{argv[1]} == "--classic-output";
  auto const selectionEventsRunfile =
    argc == 3 && std::string_view{argv[1]} == "--selection-events" ? std::string{argv[2]} : std::string{};
  auto optPrimaryFixture = std::optional<std::filesystem::path>{};

  auto const seekSteps = argc == 3 && std::string_view{argv[1]} == "--seek-steps";
  auto optSeekRangeMode = std::optional<ao::winui::ShellMode>{};

  if (argc == 3 && (seekSteps || std::string_view{argv[1]} == "--primary-play"))
  {
    optPrimaryFixture = ao::utility::pathFromUtf8(argv[2]);
  }
  else if (argc == 4 && std::string_view{argv[1]} == "--seek-range")
  {
    if (auto const mode = std::string_view{argv[2]}; mode == "classic" || mode == "modern")
    {
      optSeekRangeMode = mode == "classic" ? ao::winui::ShellMode::Classic : ao::winui::ShellMode::Modern;
      optPrimaryFixture = ao::utility::pathFromUtf8(argv[3]);
    }
  }

  if (argc != 1 && !seedAlbumColumns && !classicOutput && !optPrimaryFixture && selectionEventsRunfile.empty())
  {
    if (argc == 3 && std::string_view{argv[1]} == "--list-preview")
    {
      if (auto const locale = std::string_view{argv[2]};
          std::ranges::contains(ao::i18n::availableCatalogLocales(), locale, &ao::i18n::CatalogLocale::tag))
      {
        optListPreviewLocale = locale;
      }
    }

    if (!optListPreviewLocale)
    {
      std::cerr << "Usage: ao_winui_automation_probe.exe [--classic-output | --list-preview <packaged locale> | "
                   "--primary-play <absolute private .wav> | --seek-steps <private PCM .wav> | "
                   "--seek-range <classic|modern> <private PCM .wav> | --seed-album-columns | "
                   "--selection-events <Bazel runfile>]\n"
                   "Seek modes require an absolute private PCM .wav longer than 11s, at most 16 MiB.\n"
                   "The List preview mode leaves the dialog open for capture; choose Cancel after LIST_PREVIEW_READY.\n"
                   "--seed-album-columns prints a new temporary fixture root for process-restart checks; the caller "
                   "owns cleanup.\n";
      return 2;
    }
  }

  if (::DWORD session = 0;
      !seedAlbumColumns && (::ProcessIdToSessionId(::GetCurrentProcessId(), &session) == 0 || session == 0 ||
                            session != ::WTSGetActiveConsoleSessionId() || !hasInteractiveUserToken()))
  {
    std::cerr << "WinUI naming audit requires the active authenticated interactive console; no GUI was started.\n";
    return 2;
  }

  try
  {
    if (seedAlbumColumns)
    {
      return seedAlbumColumnsFixture();
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    std::int32_t result = 1;
    Application::Start(
      [&result,
       optListPreviewLocale,
       classicOutput,
       optPrimaryFixture,
       seekSteps,
       optSeekRangeMode,
       selectionEventsRunfile](auto&&)
      {
        std::ignore = winrt::make<WinUiAutomationProbe>(result,
                                                        optListPreviewLocale,
                                                        classicOutput,
                                                        optPrimaryFixture,
                                                        seekSteps,
                                                        optSeekRangeMode,
                                                        selectionEventsRunfile);
      });
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

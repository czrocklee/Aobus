// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// A real loaded-XAML probe for the native track table. It builds one genuine
// runtime graph (offline core library rows committed through the public
// library writer, then the real AppRuntime over a dispatcher executor) and
// publishes the shipped track.table component through the production
// ComponentRegistry, LayoutHost, and generation gate. It also composes the real
// navigation pane and presentation control to check native chrome clearance.
// All interaction is programmatic native XAML observation: no physical
// keyboard or pointer input is claimed. Expected contract failures are
// reported and aggregated with a nonzero exit status; one global 90-second
// dispatcher timeout bounds the run, and no expected failure aborts.

#include "XamlMetaDataProvider.h"
#include "app/DispatcherQueueExecutor.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/FocusedDetail.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutHost.h"
#include "layout/runtime/ResourceLookup.h"
#include "platform/StringResources.h"
#include "theme/SurfaceBrushes.h"
#include "theme/ThemeCoordinator.h"
#include "track/TrackListController.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/winui/Theme.h>
#include <ao/winui/layout/LayoutSchema.h>
#include <ao/winui/layout/ShellState.h>

#include <unknwn.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
  using ProjectedTrackRow = winrt::Aobus::TrackRowItem;
  using Controls::Border;
  using Controls::Button;
  using Controls::Grid;
  using Controls::ItemsStackPanel;
  using Controls::ListView;
  using Controls::ScrollIntoViewAlignment;
  using DispatcherTimer = winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer;

  // This probe lives outside namespace ao, so the value types it names carry
  // their owner directly rather than relying on an enclosing namespace.
  using ao::BitDepth;
  using ao::Bitrate;
  using ao::Channels;
  using ao::SampleRate;
  using ao::TrackId;

  constexpr std::size_t kTrackCount = 44;
  constexpr std::size_t kTracksPerAlbum = 11;
  constexpr std::size_t kAlbumCount = kTrackCount / kTracksPerAlbum;
  constexpr auto kGlobalTimeout = std::chrono::seconds{90};
  constexpr auto kAccentTextBrushKey = std::string_view{"AccentTextFillColorPrimaryBrush"};
  constexpr auto kNavigationProbeWidths = std::to_array<double>({719.0, 720.0, 1119.0, 1120.0, 1119.0, 720.0, 719.0});

  // One fixed-height row per display item; the classic Binding path resolves
  // through the generated metadata provider, so real header and row text
  // materializes while selection and scroll stay the assertions' subject.
  constexpr auto kRowTemplateMarkup =
    std::string_view{R"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">)"
                     R"(<Grid Height="32"><TextBlock Text="{Binding Title}" VerticalAlignment="Center" /></Grid>)"
                     R"(</DataTemplate>)"};
  constexpr auto kHeaderTemplateMarkup =
    std::string_view{R"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">)"
                     R"(<Grid Height="32"><TextBlock Text="{Binding Text}" VerticalAlignment="Center" /></Grid>)"
                     R"(</DataTemplate>)"};

  std::int32_t exitCode = 1;

  /// Scenario-scoped failure collection: expected contract violations are
  /// reported and counted instead of aborting the probe run.
  class ScenarioReport final
  {
  public:
    void beginScenario(std::string_view name)
    {
      _scenario = name;
      std::cerr << std::format("WinUI track table probe: scenario '{}'\n", name);
    }

    void check(bool const condition, std::string_view observation)
    {
      if (!condition)
      {
        record(observation);
      }
    }

    void record(std::string_view detail)
    {
      std::cerr << std::format("  FAIL [{}]: {}\n", _scenario, detail);
      ++_failureCount;
    }

    std::size_t failureCount() const noexcept { return _failureCount; }

  private:
    std::string_view _scenario;
    std::size_t _failureCount = 0;
  };

  /// The probe's disposable media and state root, removed on destruction.
  class TempRoot final
  {
  public:
    TempRoot()
    {
      auto const unique = std::chrono::steady_clock::now().time_since_epoch().count();
      _root = std::filesystem::temp_directory_path() / std::format("aobus-winui-track-table-probe-{}", unique);
      std::filesystem::create_directories(_root);
    }

    TempRoot(TempRoot const&) = delete;
    TempRoot& operator=(TempRoot const&) = delete;
    TempRoot(TempRoot&&) = delete;
    TempRoot& operator=(TempRoot&&) = delete;

    ~TempRoot()
    {
      std::error_code ignored;
      std::filesystem::remove_all(_root, ignored);
    }

    std::filesystem::path const& root() const noexcept { return _root; }

  private:
    std::filesystem::path _root;
  };

  std::vector<TrackId> sortIds(std::vector<TrackId> ids)
  {
    std::ranges::sort(ids, [](TrackId const& lhs, TrackId const& rhs) { return lhs.raw() < rhs.raw(); });
    return ids;
  }

  class WinUiTrackTableProbeApplication
    : public ApplicationT<WinUiTrackTableProbeApplication, Markup::IXamlMetadataProvider>
  {
  public:
    WinUiTrackTableProbeApplication()
    {
      UnhandledException(
        [this](Item const&, UnhandledExceptionEventArgs const& arguments)
        {
          _report.record(std::format("unhandled XAML exception: {}", winrt::to_string(arguments.Message())));
          finish();
        });
      InitializeComponent();
    }

    void OnLaunched(LaunchActivatedEventArgs const& /*arguments*/)
    {
      _window = Window{};
      _window.Title(L"Aobus WinUI track table probe");
      _root = Grid{};
      _hostBorder = Border{};
      _hostBorder.Width(420.0);
      _hostBorder.Height(260.0);
      _root.Children().Append(_hostBorder);
      _titleBarSlot = Grid{};
      _window.Content(_root);
      _windowClosedRevoker = _window.Closed(winrt::auto_revoke,
                                            [this](auto const&, auto const&)
                                            {
                                              _windowClosed = true;

                                              if (!_finishing)
                                              {
                                                _report.record("the probe window was closed before the run completed");
                                                finish();
                                              }
                                            });
      _timeout = _window.DispatcherQueue().CreateTimer();
      _timeout.Interval(kGlobalTimeout);
      _timeout.IsRepeating(false);
      _timeout.Tick(
        [this](auto const&, auto const&)
        {
          if (!_finishing)
          {
            _report.record("the global 90-second dispatcher timeout expired");
            finish();
          }
        });
      _timeout.Start();
      _window.Activate();
      enqueueStep(&WinUiTrackTableProbeApplication::setupGraphStep);
    }

    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type)
    {
      return AppProvider()->GetXamlType(type);
    }

    Markup::IXamlType GetXamlType(winrt::hstring const& fullName) { return AppProvider()->GetXamlType(fullName); }

    winrt::com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return AppProvider()->GetXmlnsDefinitions(); }

  private:
    using Step = void (WinUiTrackTableProbeApplication::*)();

    void InitializeComponent()
    {
      if (_contentLoaded)
      {
        return;
      }

      _contentLoaded = true;
      winrt::Windows::Foundation::Uri resourceLocator{L"ms-appx:///App.xaml"};
      Application::LoadComponent(*this, resourceLocator);
    }

    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> AppProvider()
    {
      if (!_appProvider)
      {
        _appProvider = winrt::make_self<winrt::Aobus::implementation::XamlMetaDataProvider>();
      }

      return _appProvider;
    }

    // --- Step plumbing -----------------------------------------------------

    void enqueueStep(Step step)
    {
      auto const queued = _window.DispatcherQueue().TryEnqueue([this, step] { runStep(step); });

      if (!queued)
      {
        _report.record("the dispatcher rejected the next probe step");
        finish();
      }
    }

    void runStep(Step step)
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
        _report.record(std::format("unexpected WinRT failure in a probe step: {} ({:#x})",
                                   winrt::to_string(error.message()),
                                   static_cast<std::uint32_t>(error.code().value)));
        finish();
      }
      catch (std::exception const& error)
      {
        _report.record(std::format("unexpected failure in a probe step: {}", error.what()));
        finish();
      }
    }

    void afterNextLayoutStep(Step step)
    {
      _layoutRevoker = _root.LayoutUpdated(winrt::auto_revoke,
                                           [this, step](Item const&, Item const&)
                                           {
                                             _layoutRevoker.revoke();
                                             enqueueStep(step);
                                           });
    }

    /// The public executor delivers posted view-service notifications ahead
    /// of a deferred checkpoint, so the next step observes genuine delivery.
    void afterPostedNotifications(Step step)
    {
      _executorPtr->defer([this, step] { enqueueStep(step); });
    }

    void failSetup(std::string_view detail)
    {
      _report.record(detail);
      finish();
    }

    // --- Observation helpers ----------------------------------------------

    std::vector<TrackId> selectedNativeTrackIds() const
    {
      auto ids = std::vector<TrackId>{};

      for (auto const& selected : _listView.SelectedItems())
      {
        if (auto const row = selected.try_as<ProjectedTrackRow>(); row && !row.IsGroupHeader() && row.TrackId() != 0)
        {
          ids.push_back(TrackId{row.TrackId()});
        }
      }

      std::ranges::sort(ids, [](TrackId const& lhs, TrackId const& rhs) { return lhs.raw() < rhs.raw(); });
      return ids;
    }

    bool nativeSelectionHasHeader() const
    {
      for (auto const& selected : _listView.SelectedItems())
      {
        if (auto const row = selected.try_as<ProjectedTrackRow>(); row && row.IsGroupHeader())
        {
          return true;
        }
      }

      return false;
    }

    bool everySelectedNativeTrackIsSeeded() const
    {
      for (auto const& selected : _listView.SelectedItems())
      {
        if (auto const row = selected.try_as<ProjectedTrackRow>(); row && !row.IsGroupHeader())
        {
          if (auto const found = std::ranges::binary_search(_sortedSeedIds,
                                                            TrackId{row.TrackId()},
                                                            [](TrackId const& lhs, TrackId const& rhs)
                                                            { return lhs.raw() < rhs.raw(); });
              !found)
          {
            return false;
          }
        }
      }

      return true;
    }

    Item firstHeaderItem()
    {
      auto const& items = _trackListPtr->items();

      for (std::uint32_t index = 0; index < items.Size(); ++index)
      {
        auto const row = items.GetAt(index).try_as<ProjectedTrackRow>();

        if (row && row.IsGroupHeader())
        {
          return items.GetAt(index);
        }
      }

      return nullptr;
    }

    std::optional<TrackId> topVisibleTrackId() const
    {
      auto const firstVisible = _rowsPanel.FirstVisibleIndex();

      if (firstVisible < 0)
      {
        return std::nullopt;
      }

      auto const& items = _trackListPtr->items();

      for (auto index = firstVisible; index < static_cast<std::int32_t>(items.Size()); ++index)
      {
        auto const row = items.GetAt(static_cast<std::uint32_t>(index)).try_as<ProjectedTrackRow>();

        if (row && !row.IsGroupHeader() && row.TrackId() != 0)
        {
          return TrackId{row.TrackId()};
        }
      }

      return std::nullopt;
    }

    bool isDisplayIndexVisible(std::size_t displayIndex) const
    {
      auto const first = _rowsPanel.FirstVisibleIndex();
      auto const last = _rowsPanel.LastVisibleIndex();
      return first >= 0 && last >= first && displayIndex >= static_cast<std::size_t>(first) &&
             displayIndex <= static_cast<std::size_t>(last);
    }

    void clearSelectionPublications() { _selectionPublications.clear(); }

    void verifyRestoration(std::vector<TrackId> const& expectedIds)
    {
      _report.check(
        _selectionPublications.empty(), "the native source replacement published no runtime selection change");
      _report.check(sortIds(_trackListPtr->selection()) == sortIds(expectedIds),
                    "the runtime selection survived the source replacement");
      _report.check(selectedNativeTrackIds() == sortIds(expectedIds),
                    std::format("the native selection was restored to {} row(s) by stable id", expectedIds.size()));
      _report.check(!nativeSelectionHasHeader(), "no group header remains in the native selection");
      _report.check(everySelectedNativeTrackIsSeeded(), "the native selection contains no phantom track ids");
    }

    // --- Steps -------------------------------------------------------------

    ao::Result<> seedCoreLibrary(std::filesystem::path const& musicRoot)
    {
      auto const databasePath = ao::rt::LibraryPaths{musicRoot}.databasePath();
      std::filesystem::create_directories(databasePath);

      auto libraryRes = ao::library::MusicLibrary::open(musicRoot, databasePath);

      if (!libraryRes)
      {
        return std::unexpected{libraryRes.error()};
      }

      auto writableRes = ao::library::WritableMusicLibrary::acquire(*libraryRes);

      if (!writableRes)
      {
        return std::unexpected{writableRes.error()};
      }

      {
        auto transaction = writableRes->writeTransaction();
        auto applyRes = transaction.apply(
          [this](ao::library::LibraryWrite& write) -> ao::Result<>
          {
            for (std::size_t index = 0; index < kTrackCount; ++index)
            {
              // The strings must outlive the builder: it stores string views.
              auto const title = std::format("Probe Title {:02}", (index * 7) % kTrackCount);
              auto const artist = std::format("Probe Artist {}", index % 3);
              auto const albumIndex = index / kTracksPerAlbum;
              auto const album = std::format("Probe Album {}", static_cast<char>('A' + albumIndex));
              auto const uri =
                std::format("probe/album-{}/track-{:02}.flac", static_cast<char>('a' + albumIndex), index);

              auto builder = ao::library::TrackBuilder::makeEmpty();
              builder.metadata()
                .title(title)
                .artist(artist)
                .album(album)
                .year(static_cast<std::uint16_t>(2001 + (index % 4)))
                .trackNumber(static_cast<std::uint16_t>((index % kTracksPerAlbum) + 1))
                .discNumber(1);
              builder.property()
                .uri(uri)
                .duration(std::chrono::seconds{120 + static_cast<std::int64_t>(index)})
                .bitrate(Bitrate{320000})
                .sampleRate(SampleRate{44100})
                .channels(Channels{2})
                .bitDepth(BitDepth{16});

              auto createRes = write.tracks().create(builder, ao::library::FileManifestBuilder::makeEmpty());

              if (!createRes)
              {
                return std::unexpected{createRes.error()};
              }

              _seededTrackIds.push_back(*createRes);
            }

            return {};
          });

        if (!applyRes)
        {
          return std::unexpected{applyRes.error()};
        }

        auto commitRes = transaction.commit();

        if (!commitRes)
        {
          return std::unexpected{commitRes.error()};
        }
      }

      // The core writer session and library close here, before the runtime
      // opens the same database through its own graph.
      return {};
    }

    void setupGraphStep()
    {
      _report.beginScenario("real runtime graph and shipped track table build");

      // The production composition root configures MRT before any controller
      // or header resolves resource text; the probe owns the same one-time
      // setup and refuses to continue when it fails.
      auto const resourceLanguageRes = ao::winui::configureResourceLanguage("en");

      if (!resourceLanguageRes)
      {
        failSetup(std::format("could not configure the English MRT context: {}", resourceLanguageRes.error().message));
        return;
      }

      try
      {
        _tempRoot.emplace();
      }
      catch (std::exception const& error)
      {
        failSetup(std::format("could not create the probe's temporary root: {}", error.what()));
        return;
      }

      auto seedRes = seedCoreLibrary(_tempRoot->root());

      if (!seedRes)
      {
        failSetup(std::format("core library seeding failed: {}", seedRes.error().message));
        return;
      }

      _sortedSeedIds = sortIds(_seededTrackIds);

      auto controllerCatalogRes = ao::i18n::MessageCatalog::create("en");

      if (!controllerCatalogRes)
      {
        failSetup(
          std::format("could not create the English controller catalog: {}", controllerCatalogRes.error().message));
        return;
      }

      auto registrationCatalogRes = ao::i18n::MessageCatalog::create("en");

      if (!registrationCatalogRes)
      {
        failSetup(
          std::format("could not create the English registration catalog: {}", registrationCatalogRes.error().message));
        return;
      }

      auto executorPtr = std::make_unique<ao::winui::DispatcherQueueExecutor>(_window.DispatcherQueue());
      auto* runtimeExecutor = executorPtr.get();

      auto workspaceStorePtr = std::make_unique<ao::rt::ConfigStore>(
        ao::rt::LibraryPaths{_tempRoot->root()}.databasePath() / "workspace.yaml");

      auto runtimeRes = ao::rt::AppRuntime::create(ao::rt::AppRuntimeDependencies{
        .executorPtr = std::move(executorPtr),
        .musicRoot = _tempRoot->root(),
        .databasePath = ao::rt::LibraryPaths{_tempRoot->root()}.databasePath(),
        .workspaceConfigStorePtr = std::move(workspaceStorePtr),
      });

      if (!runtimeRes)
      {
        failSetup(std::format("AppRuntime creation failed: {}", runtimeRes.error().message));
        return;
      }

      _runtimePtr = std::make_unique<ao::rt::AppRuntime>(std::move(*runtimeRes));
      // Publish the borrow only after the runtime owns the executor in final storage.
      _executorPtr = runtimeExecutor;
      _columnLayoutsPtr = std::make_unique<ao::uimodel::TrackColumnLayouts>(_runtimePtr->library().changes());
      _trackListPtr = std::make_unique<ao::winui::TrackListController>(_runtimePtr->views(),
                                                                       _runtimePtr->workspace(),
                                                                       _runtimePtr->library(),
                                                                       *_columnLayoutsPtr,
                                                                       std::move(*controllerCatalogRes));
      _selectionSub =
        _runtimePtr->views().onSelectionChanged([this](ao::rt::ViewService::SelectionChanged const& change)
                                                { _selectionPublications.push_back(change.selection); });

      _schemaPtr = std::make_unique<ao::uimodel::LayoutSchema>(ao::winui::layoutSchema());
      _registryPtr = std::make_unique<ao::winui::layout::ComponentRegistry>(*_schemaPtr, _actions);

      ao::winui::layout::registerTrackTableComponent(
        *_registryPtr,
        *_trackListPtr,
        [](ao::rt::ViewId, TrackId) -> ao::Result<>
        { return ao::makeError(ao::Error::Code::NotSupported, "the track table probe does not play"); },
        {},
        {},
        {},
        {},
        _actions,
        [](std::string_view) -> std::string { return {}; },
        std::move(*registrationCatalogRes),
        [this](std::string message) { _statusMessages.push_back(std::move(message)); });

      _resources = ResourceDictionary{};
      _resources.Insert(winrt::box_value(L"TrackRowTemplate"),
                        Markup::XamlReader::Load(winrt::to_hstring(kRowTemplateMarkup)).as<DataTemplate>());
      _resources.Insert(winrt::box_value(L"TrackHeaderCellTemplate"),
                        Markup::XamlReader::Load(winrt::to_hstring(kHeaderTemplateMarkup)).as<DataTemplate>());

      enqueueStep(&WinUiTrackTableProbeApplication::buildAndPublishStep);
    }

    void buildAndPublishStep()
    {
      auto const groupedRes = _trackListPtr->selectPresentation("albums");

      if (!groupedRes)
      {
        failSetup(std::format("grouped presentation selection failed: {}", groupedRes.error().message));
        return;
      }

      _hostPtr = std::make_unique<ao::winui::layout::LayoutHost>(_hostBorder);
      _gatePtr = _hostPtr->stage();
      _focusedDetailPtr = std::make_shared<ao::winui::layout::FocusedDetail>();

      auto context = ao::winui::layout::LayoutBuildContext{
        .resources = _resources,
        .surfaceBrush = ao::winui::makeSurfaceBrushResolver(std::nullopt),
        .shellState = _shellState,
        .windowActivity = _windowActivity,
        .statusMessage = _statusMessage,
        .gatePtr = _gatePtr,
        .outputDeviceIntent = ao::uimodel::OutputDeviceIntent::discarded(),
        .focusedDetailPtr = _focusedDetailPtr,
        .titleBarSlot = _titleBarSlot,
      };

      auto const node = ao::uimodel::LayoutNode{.id = "probe-track-table", .type = "track.table"};
      auto builtRes = _registryPtr->build(context, node);

      if (!builtRes)
      {
        failSetup(std::format("track.table build failed: {}", builtRes.error().message));
        return;
      }

      auto placed = std::move(*builtRes);
      auto candidate = ao::winui::layout::ShellGeneration{
        .rootPtr = std::move(placed.componentPtr),
        .gatePtr = _gatePtr,
        .focusedDetailPtr = _focusedDetailPtr,
        .titleBarElement = nullptr,
      };

      auto publishRes = _hostPtr->publish(std::move(candidate));

      if (!publishRes)
      {
        failSetup(std::format("track.table publication failed: {}", publishRes.error().message));
        return;
      }

      afterNextLayoutStep(&WinUiTrackTableProbeApplication::verifyLoadedTableStep);
    }

    void verifyLoadedTableStep()
    {
      _report.beginScenario("loaded real XAML track table with grouped rows");

      _listView = findListView(_hostBorder);

      if (!_listView)
      {
        failSetup("the published generation contains no ListView");
        return;
      }

      _rowsPanel = _listView.ItemsPanelRoot().try_as<ItemsStackPanel>();

      if (!_rowsPanel)
      {
        failSetup("the ListView has no ItemsStackPanel panel root after layout");
        return;
      }

      _report.check(_trackListPtr->rowCount() == kTrackCount,
                    std::format("the runtime projection reports {} rows", _trackListPtr->rowCount()));

      auto const& items = _trackListPtr->items();
      auto const displayCount = static_cast<std::size_t>(items.Size());
      _report.check(
        displayCount == kTrackCount + kAlbumCount,
        std::format("expected {} display items with group headers, saw {}", kTrackCount + kAlbumCount, displayCount));

      auto headerSeen = false;
      auto trackRowSeen = false;

      for (std::uint32_t index = 0; index < items.Size(); ++index)
      {
        auto const row = items.GetAt(index).try_as<ProjectedTrackRow>();

        if (!row)
        {
          _report.record("a display item did not project to Aobus.TrackRowItem");
          break;
        }

        if (row.IsGroupHeader())
        {
          headerSeen = true;
        }
        else
        {
          trackRowSeen = true;

          if (auto const found = std::ranges::binary_search(_sortedSeedIds,
                                                            TrackId{row.TrackId()},
                                                            [](TrackId const& lhs, TrackId const& rhs)
                                                            { return lhs.raw() < rhs.raw(); });
              !found)
          {
            _report.record("a grouped display row named an unseeded track id");
          }
        }
      }

      _report.check(headerSeen, "the grouped projection exposes group headers");
      _report.check(trackRowSeen, "the grouped projection exposes track rows");
      _report.check(_rowsPanel.FirstVisibleIndex() >= 0, "the loaded viewport realizes rows");

      clearSelectionPublications();
      enqueueStep(&WinUiTrackTableProbeApplication::nativeSelectionStep);
    }

    void nativeSelectionStep()
    {
      _report.beginScenario("native selection publishes through the real view service");

      _firstTrack = _seededTrackIds[3];
      auto const optDisplayIndex = _trackListPtr->displayIndexOfTrack(_firstTrack);

      if (!optDisplayIndex)
      {
        failSetup("the seeded probe track has no display index");
        return;
      }

      auto const firstItem = _trackListPtr->items().GetAt(static_cast<std::uint32_t>(*optDisplayIndex));
      _listView.SelectedItems().Clear();
      _listView.SelectedItem(firstItem);

      _report.check(
        _selectionPublications.size() == 1,
        std::format("a plain native selection published {} runtime changes", _selectionPublications.size()));
      _report.check(
        !_selectionPublications.empty() && sortIds(_selectionPublications.front()) == std::vector<TrackId>{_firstTrack},
        "the plain native selection published exactly the chosen track id");
      _report.check(sortIds(_trackListPtr->selection()) == std::vector<TrackId>{_firstTrack},
                    "the real view service holds the published selection");

      clearSelectionPublications();

      auto const sortedRes = _trackListPtr->toggleSort(ao::rt::TrackSortField::Title);

      if (!sortedRes)
      {
        failSetup(std::format("toggleSort failed: {}", sortedRes.error().message));
        return;
      }

      afterNextLayoutStep(&WinUiTrackTableProbeApplication::sortRestoreStep);
    }

    void sortRestoreStep()
    {
      _report.beginScenario("stable track-id selection restoration without runtime publication (sort)");
      verifyRestoration({_firstTrack});

      clearSelectionPublications();

      auto const resizedRes = _trackListPtr->resizeColumn("title", 48.0);

      if (!resizedRes)
      {
        failSetup(std::format("resizeColumn failed: {}", resizedRes.error().message));
        return;
      }

      afterNextLayoutStep(&WinUiTrackTableProbeApplication::widthRestoreStep);
    }

    void widthRestoreStep()
    {
      _report.beginScenario("stable track-id selection restoration without runtime publication (width)");
      verifyRestoration({_firstTrack});

      _report.beginScenario("mixed native selection publishes only valid track ids");

      _secondTrack = _seededTrackIds[7];
      auto const optSecondIndex = _trackListPtr->displayIndexOfTrack(_secondTrack);

      if (!optSecondIndex)
      {
        failSetup("the second probe track has no display index");
        return;
      }

      auto const secondItem = _trackListPtr->items().GetAt(static_cast<std::uint32_t>(*optSecondIndex));
      _listView.SelectedItems().Append(secondItem);

      _report.check(_selectionPublications.size() == 1,
                    std::format("the mixed range published {} runtime changes", _selectionPublications.size()));
      _report.check(!_selectionPublications.empty() &&
                      sortIds(_selectionPublications.front()) == sortIds({_firstTrack, _secondTrack}),
                    "the mixed native range published exactly the valid track ids");
      _report.check(sortIds(_trackListPtr->selection()) == sortIds({_firstTrack, _secondTrack}),
                    "the real view service holds both selected track ids");

      clearSelectionPublications();

      _headerItem = firstHeaderItem();

      if (!_headerItem)
      {
        failSetup("the grouped projection exposed no group header to select");
        return;
      }

      _listView.SelectedItems().Append(_headerItem);

      _report.check(
        _selectionPublications.empty(), "adding a group header to a valid selection publishes no runtime change");
      _report.check(
        !nativeSelectionHasHeader(), "the non-selectable group header was pruned from the native selection");
      _report.check(_listView.SelectedItems().Size() == 2,
                    std::format("the native selection holds {} item(s) after the header gesture",
                                _listView.SelectedItems().Size()));

      enqueueStep(&WinUiTrackTableProbeApplication::headerOnlyStep);
    }

    void headerOnlyStep()
    {
      _report.beginScenario("a header-only native choice keeps the previous runtime selection");

      clearSelectionPublications();
      _listView.SelectedItem(_headerItem);

      _report.check(_selectionPublications.empty(), "a header-only choice published no runtime selection change");
      _report.check(sortIds(_trackListPtr->selection()) == sortIds({_firstTrack, _secondTrack}),
                    "the previous runtime selection was not cleared by the header-only choice");
      _report.check(!nativeSelectionHasHeader(), "the non-selectable header was pruned from the native selection");
      _report.check(_listView.SelectedItems().Size() == 2,
                    std::format("the native selection was restored to the previous runtime rows, saw {} item(s)",
                                _listView.SelectedItems().Size()));
      _report.check(selectedNativeTrackIds() == sortIds({_firstTrack, _secondTrack}),
                    "the restored native selection names the previous runtime track ids");

      enqueueStep(&WinUiTrackTableProbeApplication::clearAndSelectAllStep);
    }

    void clearAndSelectAllStep()
    {
      _report.beginScenario("a genuine native clear empties the runtime selection");

      clearSelectionPublications();
      _listView.SelectedItems().Clear();

      _report.check(_selectionPublications.size() == 1 && _selectionPublications.front().empty(),
                    "a genuine clear published exactly one empty selection");
      _report.check(_trackListPtr->selection().empty(), "the runtime selection is empty after the genuine clear");
      _report.check(_listView.SelectedItems().Size() == 0, "the native selection is empty after the genuine clear");

      _report.beginScenario("SelectAll publishes valid ids and filters the native selection");

      clearSelectionPublications();
      _listView.SelectAll();

      _report.check(_selectionPublications.size() == 1,
                    std::format("SelectAll published {} runtime changes", _selectionPublications.size()));
      _report.check(!_selectionPublications.empty() && sortIds(_selectionPublications.front()) == _sortedSeedIds,
                    "SelectAll published exactly the valid track ids");
      _report.check(sortIds(_trackListPtr->selection()) == _sortedSeedIds,
                    "the real view service holds every valid track id after SelectAll");
      _report.check(!nativeSelectionHasHeader(), "SelectAll leaves no group header in the native selection");
      _report.check(
        _listView.SelectedItems().Size() == kTrackCount,
        std::format("the native selection holds {} item(s) after SelectAll", _listView.SelectedItems().Size()));

      clearSelectionPublications();

      auto const resizedRes = _trackListPtr->resizeColumn("title", 24.0);

      if (!resizedRes)
      {
        failSetup(std::format("resizeColumn before SelectAll restoration failed: {}", resizedRes.error().message));
        return;
      }

      afterNextLayoutStep(&WinUiTrackTableProbeApplication::selectAllRestoreStep);
    }

    void selectAllRestoreStep()
    {
      _report.beginScenario("SelectAll restoration keeps exactly the valid rows");
      verifyRestoration(_sortedSeedIds);

      _anchorTrack = _seededTrackIds[22];
      auto const optAnchorIndex = _trackListPtr->displayIndexOfTrack(_anchorTrack);

      if (!optAnchorIndex)
      {
        failSetup("the anchor probe track has no display index");
        return;
      }

      auto const anchorItem = _trackListPtr->items().GetAt(static_cast<std::uint32_t>(*optAnchorIndex));
      _listView.ScrollIntoView(anchorItem, ScrollIntoViewAlignment::Leading);

      afterNextLayoutStep(&WinUiTrackTableProbeApplication::anchorTopStep);
    }

    void anchorTopStep()
    {
      _report.beginScenario("the top visible track stays anchored across a source replacement");

      auto const optTopTrack = topVisibleTrackId();
      _report.check(optTopTrack.has_value() && *optTopTrack == _anchorTrack,
                    "the leading scroll exposed the anchor track at the top of the viewport");

      clearSelectionPublications();

      auto const sortedRes = _trackListPtr->toggleSort(ao::rt::TrackSortField::Title);

      if (!sortedRes)
      {
        failSetup(std::format("the anchor refresh sort failed: {}", sortedRes.error().message));
        return;
      }

      afterNextLayoutStep(&WinUiTrackTableProbeApplication::anchorAfterSortStep);
    }

    void anchorAfterSortStep()
    {
      auto const optTopTrack = topVisibleTrackId();
      _report.check(optTopTrack.has_value() && *optTopTrack == _anchorTrack,
                    "the top visible track kept its stable id across the source replacement");
      _report.check(
        _selectionPublications.empty(), "the anchored source replacement published no runtime selection change");

      enqueueStep(&WinUiTrackTableProbeApplication::revealStep);
    }

    void revealStep()
    {
      _report.beginScenario("an explicit reveal takes priority over the top anchor");

      _revealTrackId = _seededTrackIds[30];
      clearSelectionPublications();

      auto const revealRes =
        _trackListPtr->revealTrack(_revealTrackId, _trackListPtr->viewId(), _trackListPtr->activeListId());

      if (!revealRes)
      {
        failSetup(std::format("revealTrack failed: {}", revealRes.error().message));
        return;
      }

      _report.check(_selectionPublications.size() == 1 &&
                      sortIds(_selectionPublications.front()) == std::vector<TrackId>{_revealTrackId},
                    "the reveal published its own selection exactly once");
      clearSelectionPublications();

      afterNextLayoutStep(&WinUiTrackTableProbeApplication::revealVisibleStep);
    }

    void revealVisibleStep()
    {
      auto const optRevealIndex = _trackListPtr->displayIndexOfTrack(_revealTrackId);
      _report.check(optRevealIndex.has_value(), "the revealed track has a display index");

      auto const selectedRow = _listView.SelectedItem().try_as<ProjectedTrackRow>();
      _report.check(
        selectedRow && selectedRow.TrackId() == _revealTrackId.raw(), "the revealed row is the native selected item");

      if (optRevealIndex)
      {
        _report.check(isDisplayIndexVisible(*optRevealIndex),
                      "the revealed row is visible, taking priority over the scroll anchor");
      }

      auto const topItem = _trackListPtr->items().GetAt(0);
      _listView.ScrollIntoView(topItem, ScrollIntoViewAlignment::Leading);

      afterNextLayoutStep(&WinUiTrackTableProbeApplication::dedupeScrolledStep);
    }

    void dedupeScrolledStep()
    {
      _report.beginScenario("a processed reveal does not replay on a later refresh");

      auto const optRevealIndex = _trackListPtr->displayIndexOfTrack(_revealTrackId);

      if (optRevealIndex)
      {
        _report.check(!isDisplayIndexVisible(*optRevealIndex), "the view scrolled away from the revealed row");
      }

      clearSelectionPublications();

      auto const resizedRes = _trackListPtr->resizeColumn("title", -24.0);

      if (!resizedRes)
      {
        failSetup(std::format("the dedupe refresh resize failed: {}", resizedRes.error().message));
        return;
      }

      afterNextLayoutStep(&WinUiTrackTableProbeApplication::dedupeRestoreStep);
    }

    void dedupeRestoreStep()
    {
      auto const optRevealIndex = _trackListPtr->displayIndexOfTrack(_revealTrackId);

      if (optRevealIndex)
      {
        _report.check(
          !isDisplayIndexVisible(*optRevealIndex), "the processed reveal did not re-scroll to the revealed row");
      }

      auto const selectedRow = _listView.SelectedItem().try_as<ProjectedTrackRow>();
      _report.check(selectedRow && selectedRow.TrackId() == _revealTrackId.raw(),
                    "the revealed selection survived the later source replacement");
      _report.check(
        _selectionPublications.empty(), "the later source replacement published no runtime selection change");

      enqueueStep(&WinUiTrackTableProbeApplication::a15PresentationStep);
    }

    // --- Presentation and transport probe steps -----------------------------

    ao::winui::layout::LayoutBuildContext makeBuildContext()
    {
      return ao::winui::layout::LayoutBuildContext{
        .resources = _resources,
        .surfaceBrush = ao::winui::makeSurfaceBrushResolver(std::nullopt),
        .shellState = _shellState,
        .windowActivity = _windowActivity,
        .statusMessage = _statusMessage,
        .gatePtr = _gatePtr,
        .outputDeviceIntent = ao::uimodel::OutputDeviceIntent::discarded(),
        .focusedDetailPtr = _focusedDetailPtr,
        .titleBarSlot = _titleBarSlot,
      };
    }

    bool isSameInstance(Item const& lhs, Item const& rhs)
    {
      if (!lhs || !rhs)
      {
        return false;
      }

      // Content properties and projected interfaces can expose different ABI
      // pointers for the same object. Query its canonical COM identity first.
      return lhs.as<::IUnknown>().get() == rhs.as<::IUnknown>().get();
    }

    /// Everything the probe reads off one freshly built transport button.
    struct TransportVisuals final
    {
      bool hasButton = false;
      bool isEnabled = false;
      bool hasSymbolIcon = false;
      std::uint32_t symbolValue = 0;
      bool foregroundUnset = false;
      Item foreground{nullptr};
      std::string tooltip;
      std::string automationName;
    };

    TransportVisuals captureTransportVisuals(std::string_view commandId)
    {
      auto visuals = TransportVisuals{};
      auto const node = ao::uimodel::LayoutNode{
        .id = "probe-transport-button",
        .type = "playback.transportButton",
        .props = {{"command", ao::uimodel::LayoutValue{std::string{commandId}}}},
      };
      auto context = makeBuildContext();
      auto builtRes = _registryPtr->build(context, node);

      if (!builtRes)
      {
        _report.record(std::format("playback.transportButton build failed: {}", builtRes.error().message));
        return visuals;
      }

      auto placed = std::move(*builtRes);
      _transportButtonPtr = std::move(placed.componentPtr);
      _transportButton = _transportButtonPtr->element().as<Controls::Button>();
      visuals.hasButton = true;
      visuals.isEnabled = _transportButton.IsEnabled();

      if (auto const icon = _transportButton.Content().try_as<Controls::SymbolIcon>(); icon)
      {
        visuals.hasSymbolIcon = true;
        visuals.symbolValue = static_cast<std::uint32_t>(icon.Symbol());
      }

      visuals.foregroundUnset = _transportButton.ReadLocalValue(Controls::Control::ForegroundProperty()) ==
                                winrt::Microsoft::UI::Xaml::DependencyProperty::UnsetValue();
      visuals.foreground = _transportButton.Foreground().as<Item>();
      visuals.tooltip = winrt::to_string(winrt::unbox_value_or<winrt::hstring>(
        Controls::ToolTipService::GetToolTip(_transportButton), winrt::hstring{}));
      visuals.automationName =
        winrt::to_string(winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::GetName(_transportButton));
      return visuals;
    }

    /// The full button text, tooltip, and automation name must agree.
    void checkPresentationButton(std::string_view expectedText)
    {
      auto const label =
        winrt::to_string(winrt::unbox_value_or<winrt::hstring>(_presentationButton.Content(), winrt::hstring{}));
      _report.check(label == expectedText,
                    std::format("the presentation button shows '{}' but expected '{}'", label, expectedText));

      auto const tooltip = winrt::to_string(winrt::unbox_value_or<winrt::hstring>(
        Controls::ToolTipService::GetToolTip(_presentationButton), winrt::hstring{}));
      auto const automationName =
        winrt::to_string(winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::GetName(_presentationButton));
      _report.check(tooltip == expectedText && automationName == expectedText,
                    std::format("the full text '{}' is shared by tooltip '{}' and automation name '{}'",
                                expectedText,
                                tooltip,
                                automationName));
    }

    void a15PresentationStep()
    {
      _report.beginScenario("presentation button labels the active presentation and the column sort");

      auto trackCatalogRes = ao::i18n::MessageCatalog::create("en");

      if (!trackCatalogRes)
      {
        failSetup(
          std::format("could not create the English track component catalog: {}", trackCatalogRes.error().message));
        return;
      }

      auto playbackCatalogRes = ao::i18n::MessageCatalog::create("en");

      if (!playbackCatalogRes)
      {
        failSetup(std::format(
          "could not create the English playback component catalog: {}", playbackCatalogRes.error().message));
        return;
      }

      _themeCoordinatorPtr = std::make_unique<ao::winui::ThemeCoordinator>(_tempRoot->root() / "probe-theme.yaml");
      _presentationCatalogPtr =
        std::make_unique<ao::uimodel::TrackPresentationCatalog>(_runtimePtr->workspace(), *trackCatalogRes);
      _listPresentationsPtr =
        std::make_unique<ao::uimodel::ListPresentations>(*_presentationCatalogPtr, _runtimePtr->library().changes());
      _playbackActionsPtr = std::make_unique<ao::uimodel::PlaybackActions>(_runtimePtr->playback(), [] {});

      ao::winui::layout::registerTrackComponents(*_registryPtr,
                                                 _runtimePtr->async(),
                                                 _runtimePtr->views(),
                                                 _runtimePtr->workspace(),
                                                 _runtimePtr->completion(),
                                                 _runtimePtr->resourceBytes(),
                                                 *_themeCoordinatorPtr,
                                                 *_trackListPtr,
                                                 *_presentationCatalogPtr,
                                                 *_listPresentationsPtr,
                                                 {},
                                                 std::move(*trackCatalogRes),
                                                 [this](std::string message)
                                                 { _statusMessages.push_back(std::move(message)); });
      ao::winui::layout::registerPlaybackComponents(*_registryPtr,
                                                    _runtimePtr->async(),
                                                    _runtimePtr->playback(),
                                                    *_playbackActionsPtr,
                                                    _runtimePtr->resourceBytes(),
                                                    *_themeCoordinatorPtr,
                                                    std::move(*playbackCatalogRes),
                                                    _shellStateChanged,
                                                    _windowActivityChanged);

      auto const buttonNode =
        ao::uimodel::LayoutNode{.id = "probe-presentation-button", .type = "track.presentationButton"};
      auto context = makeBuildContext();
      auto builtRes = _registryPtr->build(context, buttonNode);

      if (!builtRes)
      {
        failSetup(std::format("track.presentationButton build failed: {}", builtRes.error().message));
        return;
      }

      auto placed = std::move(*builtRes);
      _presentationButtonPtr = std::move(placed.componentPtr);
      _presentationButton = _presentationButtonPtr->element().as<Controls::Button>();
      _root.Children().Append(_presentationButtonPtr->element());

      // The table scenarios left an active column sort, so the button names it.
      _report.check(_trackListPtr->activePresentationId() == "windows-column-sort",
                    "the table scenarios left the synthetic column-sort presentation id");
      checkPresentationButton("Column sort");

      auto const albumsRes = _trackListPtr->selectPresentation("albums");

      if (!albumsRes)
      {
        failSetup(std::format("selectPresentation(albums) failed: {}", albumsRes.error().message));
        return;
      }

      afterPostedNotifications(&WinUiTrackTableProbeApplication::a15NamedBaselineStep);
    }

    void a15NamedBaselineStep()
    {
      _report.check(_trackListPtr->activePresentationId() == "albums", "the named baseline presentation is active");
      checkPresentationButton("Albums");
      _presentationsBefore = _listPresentationsPtr->snapshot();

      auto const sortedRes = _trackListPtr->toggleSort(ao::rt::TrackSortField::Title);

      if (!sortedRes)
      {
        failSetup(std::format("toggleSort failed: {}", sortedRes.error().message));
        return;
      }

      afterPostedNotifications(&WinUiTrackTableProbeApplication::a15SortLabelStep);
    }

    void a15SortLabelStep()
    {
      _report.check(_trackListPtr->activePresentationId() == "windows-column-sort",
                    "a column-header sort activates the synthetic presentation id");
      checkPresentationButton("Column sort");
      _report.check(_listPresentationsPtr->snapshot() == _presentationsBefore,
                    "the sort changed no per-list presentation preference");

      auto const reversedRes = _trackListPtr->toggleSort(ao::rt::TrackSortField::Title);

      if (!reversedRes)
      {
        failSetup(std::format("the reversed toggleSort failed: {}", reversedRes.error().message));
        return;
      }

      afterPostedNotifications(&WinUiTrackTableProbeApplication::a15ReversedSortStep);
    }

    void a15ReversedSortStep()
    {
      _report.check(_trackListPtr->activePresentationId() == "windows-column-sort",
                    "reversing the sort direction keeps the synthetic presentation id");
      checkPresentationButton("Column sort");
      _report.check(_listPresentationsPtr->snapshot() == _presentationsBefore,
                    "the reversed sort changed no per-list presentation preference");

      // A genuine unknown presentation id keeps its raw identifier: only the
      // column-sort id carries localized copy.
      auto const unknownSpec = ao::rt::TrackPresentationSpec{
        .id = "probe-unknown-presentation",
        .sortBy = {{.field = ao::rt::TrackSortField::Title, .ascending = true}},
        .visibleFields = {ao::rt::TrackField::Title, ao::rt::TrackField::Artist},
      };
      auto const unknownRes = _runtimePtr->views().setPresentation(_trackListPtr->viewId(), unknownSpec);

      if (!unknownRes)
      {
        failSetup(std::format("the unknown-id setPresentation failed: {}", unknownRes.error().message));
        return;
      }

      afterPostedNotifications(&WinUiTrackTableProbeApplication::a15UnknownIdStep);
    }

    void a15UnknownIdStep()
    {
      _report.check(_trackListPtr->activePresentationId() == "probe-unknown-presentation",
                    "the genuine unknown presentation id is active");
      checkPresentationButton("probe-unknown-presentation");
      _report.check(_listPresentationsPtr->snapshot() == _presentationsBefore,
                    "the raw service transition changed no per-list presentation preference");

      auto const namedRes = _trackListPtr->selectPresentation("albums");

      if (!namedRes)
      {
        failSetup(std::format("the closing selectPresentation(albums) failed: {}", namedRes.error().message));
        return;
      }

      afterPostedNotifications(&WinUiTrackTableProbeApplication::a15ClosingNamedStep);
    }

    void a15ClosingNamedStep()
    {
      _report.check(_trackListPtr->activePresentationId() == "albums", "the closing named presentation is active");
      checkPresentationButton("Albums");
      _report.check(_listPresentationsPtr->snapshot() == _presentationsBefore,
                    "no per-list presentation preference changed through any sort or raw service transition");
      _report.check(!_listPresentationsPtr->presentationIdForList(_trackListPtr->activeListId()).has_value(),
                    "the active list retains no saved presentation preference");

      enqueueStep(&WinUiTrackTableProbeApplication::b9TransportStep);
    }

    void b9TransportStep()
    {
      _report.beginScenario("repeat transport presentation through the registered component");

      auto& commands = _runtimePtr->playback().commands();
      auto const repeatExpectedEnabled = _playbackActionsPtr->isEnabled(ao::uimodel::PlaybackCommand::CycleRepeat);

      commands.setRepeatMode(ao::rt::RepeatMode::Off);
      auto const repeatOff = captureTransportVisuals("cycleRepeat");

      if (!repeatOff.hasButton)
      {
        failSetup("the first transport button build failed");
        return;
      }

      _report.check(repeatOff.hasSymbolIcon, "the idle repeat button presents a real SymbolIcon");
      _report.check(repeatOff.foregroundUnset,
                    "an idle repeat button clears the local foreground and preserves the authored style");
      _report.check(repeatOff.isEnabled == repeatExpectedEnabled,
                    "the repeat button's enablement follows real runtime availability");
      _report.check(!repeatOff.tooltip.empty() && repeatOff.tooltip == repeatOff.automationName,
                    std::format("the repeat tooltip '{}' and automation name '{}' share their full text",
                                repeatOff.tooltip,
                                repeatOff.automationName));

      commands.setRepeatMode(ao::rt::RepeatMode::All);
      auto const repeatAll = captureTransportVisuals("cycleRepeat");
      auto const stockAccent = ao::winui::layout::lookupResource(_resources, kAccentTextBrushKey);
      _report.check(
        repeatAll.hasButton && repeatAll.hasSymbolIcon, "the engaged repeat-all button presents a real SymbolIcon");
      _report.check(isSameInstance(repeatAll.foreground, stockAccent),
                    "the engaged repeat-all foreground is the exact accent brush from the public resource lookup");
      _report.check(repeatAll.isEnabled == repeatExpectedEnabled,
                    "the engaged repeat button's enablement still follows real runtime availability");

      commands.setRepeatMode(ao::rt::RepeatMode::One);
      auto const repeatOne = captureTransportVisuals("cycleRepeat");
      _report.check(
        repeatOne.hasSymbolIcon && repeatAll.hasSymbolIcon && repeatOne.symbolValue != repeatAll.symbolValue,
        std::format("repeat-one presents symbol {:#x}, distinct from repeat-all's {:#x}",
                    repeatOne.symbolValue,
                    repeatAll.symbolValue));
      _report.check(isSameInstance(repeatOne.foreground, stockAccent),
                    "the engaged repeat-one foreground is the same exact accent brush");
      _report.check(
        repeatOff.hasSymbolIcon && repeatAll.hasSymbolIcon && repeatOff.symbolValue == repeatAll.symbolValue,
        "an idle repeat button and an engaged repeat-all button share the repeat glyph");

      // Shuffle engagement follows the same public command surface.
      commands.setShuffleMode(ao::rt::ShuffleMode::On);
      auto const shuffleOn = captureTransportVisuals("toggleShuffle");
      auto const shuffleExpectedEnabled = _playbackActionsPtr->isEnabled(ao::uimodel::PlaybackCommand::ToggleShuffle);
      _report.check(shuffleOn.hasButton && isSameInstance(shuffleOn.foreground, stockAccent),
                    "an engaged shuffle button uses the exact accent brush");
      _report.check(shuffleOn.isEnabled == shuffleExpectedEnabled,
                    "the shuffle button's enablement follows real runtime availability");

      commands.setShuffleMode(ao::rt::ShuffleMode::Off);
      auto const shuffleOff = captureTransportVisuals("toggleShuffle");
      _report.check(
        shuffleOff.hasButton && shuffleOff.foregroundUnset, "an idle shuffle button clears the local foreground");

      // An authored window-scope accent override wins for a freshly built
      // component, and removing it restores the stock Application fallback.
      auto const authoredAccent = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
        winrt::Windows::UI::Color{.A = 255, .R = 64, .G = 200, .B = 120}};
      auto const authoredIdentity = authoredAccent.as<Item>();
      _resources.Insert(winrt::box_value(winrt::to_hstring(kAccentTextBrushKey)), authoredAccent);

      commands.setRepeatMode(ao::rt::RepeatMode::All);
      auto const overridden = captureTransportVisuals("cycleRepeat");
      _report.check(overridden.hasButton && isSameInstance(overridden.foreground, authoredIdentity),
                    "a freshly built engaged button takes the authored window accent exactly");
      _report.check(
        isSameInstance(ao::winui::layout::lookupResource(_resources, kAccentTextBrushKey), authoredIdentity),
        "the public resource lookup prefers the authored window scope");

      _resources.Remove(winrt::box_value(winrt::to_hstring(kAccentTextBrushKey)));
      auto const restored = captureTransportVisuals("cycleRepeat");
      _report.check(restored.hasButton && isSameInstance(restored.foreground, stockAccent),
                    "removing the override restores the exact stock accent brush");

      // A live transition on the retained engaged button discriminates the
      // clear branch: a freshly built button is born with an unset foreground,
      // so only a previously engaged one can prove ClearValue ran.
      _liveButtonIdentity = _transportButton.as<Item>();
      _liveRepeatGlyphValue = repeatAll.symbolValue;
      _liveRepeatOneGlyphValue = repeatOne.symbolValue;
      _runtimePtr->playback().commands().setRepeatMode(ao::rt::RepeatMode::Off);
      afterPostedNotifications(&WinUiTrackTableProbeApplication::b9LiveClearedStep);
    }

    void b9LiveClearedStep()
    {
      auto const liveIdentity = _transportButton.as<Item>();
      _report.check(
        isSameInstance(liveIdentity, _liveButtonIdentity), "the live off transition kept the same button instance");
      _report.check(_transportButton.ReadLocalValue(Controls::Control::ForegroundProperty()) ==
                      winrt::Microsoft::UI::Xaml::DependencyProperty::UnsetValue(),
                    "the previously engaged button cleared its local foreground on the live off transition");

      if (auto const icon = _transportButton.Content().try_as<Controls::SymbolIcon>(); icon)
      {
        _report.check(static_cast<std::uint32_t>(icon.Symbol()) == _liveRepeatGlyphValue,
                      std::format("the live cleared button shows the repeat glyph {:#x}",
                                  static_cast<std::uint32_t>(icon.Symbol())));
      }
      else
      {
        _report.record("the live cleared button no longer presents a SymbolIcon");
      }

      auto const tooltip = winrt::to_string(winrt::unbox_value_or<winrt::hstring>(
        Controls::ToolTipService::GetToolTip(_transportButton), winrt::hstring{}));
      auto const automationName =
        winrt::to_string(winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::GetName(_transportButton));
      _report.check(
        !tooltip.empty() && tooltip == automationName,
        std::format(
          "the live cleared tooltip '{}' and automation name '{}' share their full text", tooltip, automationName));

      _runtimePtr->playback().commands().setRepeatMode(ao::rt::RepeatMode::One);
      afterPostedNotifications(&WinUiTrackTableProbeApplication::b9LiveRepeatOneStep);
    }

    void b9LiveRepeatOneStep()
    {
      auto const liveIdentity = _transportButton.as<Item>();
      _report.check(isSameInstance(liveIdentity, _liveButtonIdentity),
                    "the live all-to-one transition kept the same button instance");

      auto const stockAccent = ao::winui::layout::lookupResource(_resources, kAccentTextBrushKey);
      _report.check(isSameInstance(_transportButton.Foreground().as<Item>(), stockAccent),
                    "the live repeat-one transition restored the exact accent brush");

      if (auto const icon = _transportButton.Content().try_as<Controls::SymbolIcon>(); icon)
      {
        _report.check(static_cast<std::uint32_t>(icon.Symbol()) == _liveRepeatOneGlyphValue,
                      std::format("the live repeat-one button shows symbol {:#x}, the distinct repeat-one glyph",
                                  static_cast<std::uint32_t>(icon.Symbol())));
      }
      else
      {
        _report.record("the live repeat-one button no longer presents a SymbolIcon");
      }

      enqueueStep(&WinUiTrackTableProbeApplication::navigationComposeStep);
    }

    // --- Native navigation chrome clearance --------------------------------

    void navigationComposeStep()
    {
      _report.beginScenario("native navigation chrome leaves presentation controls clear across width transitions");
      auto catalogRes = ao::i18n::MessageCatalog::create("en");

      if (!catalogRes)
      {
        failSetup(std::format("navigation catalog creation failed: {}", catalogRes.error().message));
        return;
      }

      auto const projectionCatalog = *catalogRes;
      ao::winui::layout::registerContainerComponents(*_registryPtr);
      // Representative toolbar spacing, not an expected chrome compensation.
      _resources.Insert(
        winrt::box_value(L"ProbeNavigationToolbarStyle"),
        Markup::XamlReader::Load(L"<Style xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" "
                                 L"TargetType=\"Grid\"><Setter Property=\"Padding\" Value=\"12,8\" /></Style>"));
      ao::winui::layout::registerNavigationPaneComponent(
        *_registryPtr,
        *_trackListPtr,
        _runtimePtr->workspace(),
        [this, projectionCatalog]
        { return ao::uimodel::buildListTreeProjection(projectionCatalog, _runtimePtr->library().snapshot().lists()); },
        {},
        {},
        {},
        {},
        {},
        std::move(*catalogRes),
        {},
        _shellStateChanged,
        [this](std::string message) { _statusMessages.push_back(std::move(message)); });

      // Replace the prior table generation, not the real runtime collaborators.
      auto presentationIndex = std::uint32_t{};

      if (_root.Children().IndexOf(_presentationButton, presentationIndex))
      {
        _root.Children().RemoveAt(presentationIndex);
      }

      _presentationButton = nullptr;
      _presentationButtonPtr.reset();
      _hostBorder.HorizontalAlignment(HorizontalAlignment::Left);
      _hostBorder.Height(500.0);
      _window.AppWindow().Resize(winrt::Windows::Graphics::SizeInt32{.Width = 2000, .Height = 1200});
      _shellState = ao::winui::resolveShellState(ao::winui::ShellMode::Modern, 719.0, std::nullopt);
      _gatePtr = _hostPtr->stage();
      auto context = makeBuildContext();
      auto const node = ao::uimodel::LayoutNode{
        .id = "probe-navigation",
        .type = "windows.navigationPane",
        .props = {{"presentation", ao::uimodel::LayoutValue{std::string{"navigationView"}}}},
        .children = {{
          .id = "probe-navigation-workspace",
          .type = "box",
          .props = {{"orientation", ao::uimodel::LayoutValue{std::string{"vertical"}}}},
          .children = {{
            .id = "probe-navigation-toolbar",
            .type = "box",
            .props = {{"orientation", ao::uimodel::LayoutValue{std::string{"horizontal"}}}},
            .layout = {{"heightRequest", ao::uimodel::LayoutValue{56.0}},
                       {"styleKey", ao::uimodel::LayoutValue{std::string{"ProbeNavigationToolbarStyle"}}}},
            .children = {{.id = "probe-navigation-presentation",
                          .type = "track.presentationButton",
                          .layout = {{"widthRequest", ao::uimodel::LayoutValue{112.0}},
                                     {"valign", ao::uimodel::LayoutValue{std::string{"center"}}}}}},
          }},
        }},
      };
      auto builtRes = _registryPtr->build(context, node);

      if (!builtRes)
      {
        failSetup(std::format("navigation composition failed: {}", builtRes.error().message));
        return;
      }

      auto placed = std::move(*builtRes);
      _navigation = placed.componentPtr->element().as<Controls::NavigationView>();
      _navigationLoadedRevoker =
        _navigation.Loaded(winrt::auto_revoke,
                           [this](auto&&, auto&&)
                           {
                             _navigationLoadedRevoker.revoke();
                             enqueueStep(&WinUiTrackTableProbeApplication::navigationResizeStep);
                           });
      auto candidate = ao::winui::layout::ShellGeneration{
        .rootPtr = std::move(placed.componentPtr),
        .gatePtr = _gatePtr,
        .focusedDetailPtr = _focusedDetailPtr,
        .titleBarElement = nullptr,
      };
      auto publishRes = _hostPtr->publish(std::move(candidate));

      if (!publishRes)
      {
        failSetup(std::format("navigation publication failed: {}", publishRes.error().message));
        return;
      }

      _navigationContent = _navigation.Content().as<FrameworkElement>();
      _navigationWidthIndex = 0;
      _navigationSortChecked = false;
    }

    void navigationResizeStep()
    {
      auto const width = kNavigationProbeWidths[_navigationWidthIndex];
      _hostBorder.Width(width);
      _shellState = ao::winui::resolveShellState(ao::winui::ShellMode::Modern, width, std::nullopt);
      _shellStateChanged.emit(_shellState);
      _root.UpdateLayout();
      // UpdateLayout also covers unchanged widths; do not wait for an event
      // that a no-op setter is not required to raise on the second pass.
      enqueueStep(&WinUiTrackTableProbeApplication::navigationBoundsStep);
    }

    static Button findContentButton(DependencyObject const& node)
    {
      if (auto const button = node.try_as<Button>(); button)
      {
        return button;
      }

      auto const count = Media::VisualTreeHelper::GetChildrenCount(node);

      for (std::int32_t index = 0; index < count; ++index)
      {
        if (auto const button = findContentButton(Media::VisualTreeHelper::GetChild(node, index)); button)
        {
          return button;
        }
      }

      return nullptr;
    }

    void collectNavigationChrome(DependencyObject const& node, std::vector<Controls::Primitives::ButtonBase>& buttons)
    {
      if (isSameInstance(node.as<Item>(), _navigationContent.as<Item>()))
      {
        return;
      }

      if (auto const element = node.try_as<UIElement>(); element && element.Visibility() != Visibility::Visible)
      {
        return;
      }

      if (auto const button = node.try_as<Controls::Primitives::ButtonBase>();
          button && button.ActualWidth() > 0.0 && button.ActualHeight() > 0.0)
      {
        // Disabled Back still occupies chrome and must remain part of the oracle.
        buttons.push_back(button);
      }

      auto const count = Media::VisualTreeHelper::GetChildrenCount(node);

      for (std::int32_t index = 0; index < count; ++index)
      {
        collectNavigationChrome(Media::VisualTreeHelper::GetChild(node, index), buttons);
      }
    }

    void navigationBoundsStep()
    {
      std::ignore = _navigation.ApplyTemplate();
      _root.UpdateLayout();
      _report.check(_navigation.IsLoaded() && _navigationContent.IsLoaded(),
                    "the native navigation template and workspace are loaded into the window");

      if (_navigationLightChecked)
      {
        _report.check(_navigation.ActualTheme() == ElementTheme::Light,
                      "the same native navigation control uses the requested window-local light theme");
      }
      auto const width = kNavigationProbeWidths[_navigationWidthIndex];
      _report.check(
        std::abs(_navigation.ActualWidth() - width) < 0.01,
        std::format("native navigation width {} matches requested {} DIP", _navigation.ActualWidth(), width));
      auto const expectedMode = width < 720.0    ? Controls::NavigationViewPaneDisplayMode::LeftMinimal
                                : width < 1120.0 ? Controls::NavigationViewPaneDisplayMode::LeftCompact
                                                 : Controls::NavigationViewPaneDisplayMode::Left;
      _report.check(_navigation.PaneDisplayMode() == expectedMode,
                    std::format("native pane mode follows the expected tier at {} DIP", width));
      _report.check(_navigation.IsPaneOpen() == (width >= 1120.0), "only the expanded pane is initially open");
      _report.check(isSameInstance(_navigation.Content(), _navigationContent.as<Item>()),
                    "resizing retains the same workspace content");

      auto const button = findContentButton(_navigationContent);

      if (!button)
      {
        failSetup("the composed navigation workspace contains no presentation button");
        return;
      }

      if (!_navigationPresentationButton)
      {
        _navigationPresentationButton = button;
      }

      _report.check(isSameInstance(button.as<Item>(), _navigationPresentationButton.as<Item>()),
                    "resizing retains the same presentation button");
      _report.check(button.IsLoaded(), "the presentation control is loaded, not merely constructed");
      _report.check(
        Automation::AutomationProperties::GetName(button) == (_navigationSortChecked ? L"Column sort" : L"Albums"),
        "the newly composed button displays the actual named or posted column-sort caption");
      auto const bounds =
        button.TransformToVisual(_navigation)
          .TransformBounds(winrt::Windows::Foundation::Rect{
            0.0F, 0.0F, static_cast<float>(button.ActualWidth()), static_cast<float>(button.ActualHeight())});
      _report.check(bounds.Width > 0.0F && bounds.Height > 0.0F, "the actual presentation control has positive bounds");
      auto chrome = std::vector<Controls::Primitives::ButtonBase>{};
      collectNavigationChrome(_navigation, chrome);
      _report.check(chrome.size() >= 2, "the geometry oracle observes both Back and pane-toggle chrome");
      _report.check(std::ranges::any_of(chrome,
                                        [](auto const& nativeButton)
                                        {
                                          return Automation::AutomationProperties::GetName(nativeButton) == L"Back" &&
                                                 nativeButton.IsLoaded() && !nativeButton.IsEnabled();
                                        }),
                    "the real loaded, disabled Back button participates in the bounds oracle");

      for (auto const& nativeButton : chrome)
      {
        auto const nativeBounds =
          nativeButton.TransformToVisual(_navigation)
            .TransformBounds(winrt::Windows::Foundation::Rect{0.0F,
                                                              0.0F,
                                                              static_cast<float>(nativeButton.ActualWidth()),
                                                              static_cast<float>(nativeButton.ActualHeight())});
        bool const intersects =
          bounds.X < nativeBounds.X + nativeBounds.Width && nativeBounds.X < bounds.X + bounds.Width &&
          bounds.Y < nativeBounds.Y + nativeBounds.Height && nativeBounds.Y < bounds.Y + bounds.Height;
        _report.check(!intersects,
                      std::format("presentation and native chrome do not intersect at {} DIP: y={} / native y={}",
                                  width,
                                  bounds.Y,
                                  nativeBounds.Y));
      }

      auto const contentOrigin = _navigationContent.TransformToVisual(_navigation).TransformPoint({});

      if (width < 720.0)
      {
        _optNavigationChromeBandTop = contentOrigin.Y;
      }
      else
      {
        _report.check(_optNavigationChromeBandTop && contentOrigin.Y < *_optNavigationChromeBandTop,
                      "Compact/Expanded returns the narrow chrome band to the workspace");
      }

      if (++_navigationWidthIndex < kNavigationProbeWidths.size())
      {
        enqueueStep(&WinUiTrackTableProbeApplication::navigationResizeStep);
      }
      else if (!_navigationSortChecked)
      {
        _navigationSortChecked = true;
        auto const sortedRes = _trackListPtr->toggleSort(ao::rt::TrackSortField::Title);

        if (!sortedRes)
        {
          failSetup(std::format("navigation column-sort transition failed: {}", sortedRes.error().message));
          return;
        }

        _navigationWidthIndex = 0;
        afterPostedNotifications(&WinUiTrackTableProbeApplication::navigationResizeStep);
      }
      else if (!_navigationLightChecked)
      {
        _navigationLightChecked = true;
        _navigationWidthIndex = 0;
        _root.RequestedTheme(ElementTheme::Light);
        afterPostedNotifications(&WinUiTrackTableProbeApplication::navigationResizeStep);
      }
      else
      {
        finish();
      }
    }

    // --- Native surface discovery -------------------------------------------

    static ListView findListView(winrt::Microsoft::UI::Xaml::DependencyObject const& node)
    {
      if (auto const list = node.try_as<ListView>(); list)
      {
        return list;
      }

      auto const childCount = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(node);

      for (std::int32_t index = 0; index < childCount; ++index)
      {
        if (auto const found = findListView(winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChild(node, index));
            found)
        {
          return found;
        }
      }

      return nullptr;
    }

    // --- Teardown ------------------------------------------------------------

    void finish()
    {
      if (_finishing)
      {
        return;
      }

      _finishing = true;

      if (_timeout)
      {
        _timeout.Stop();
      }

      _layoutRevoker.revoke();
      _navigationLoadedRevoker.revoke();

      try
      {
        _selectionSub.reset();

        if (_hostPtr)
        {
          _hostPtr->retire();
        }

        _transportButtonPtr.reset();
        _presentationButtonPtr.reset();
        _playbackActionsPtr.reset();
        _listPresentationsPtr.reset();
        _presentationCatalogPtr.reset();
        _themeCoordinatorPtr.reset();

        _registryPtr.reset();
        _schemaPtr.reset();
        _trackListPtr.reset();
        _columnLayoutsPtr.reset();

        if (_executorPtr != nullptr)
        {
          _executorPtr->beginClosing();
        }

        if (_runtimePtr)
        {
          _runtimePtr->shutdown();
        }

        if (_executorPtr != nullptr)
        {
          _executorPtr->completeClosing();
        }

        _runtimePtr.reset();
        _tempRoot.reset();
        ao::winui::resetResourceLanguage();
      }
      catch (winrt::hresult_error const& error)
      {
        std::cerr << std::format("  FAIL [teardown]: {}\n", winrt::to_string(error.message()));
        ++_teardownFailures;
      }
      catch (std::exception const& error)
      {
        std::cerr << std::format("  FAIL [teardown]: {}\n", error.what());
        ++_teardownFailures;
      }

      auto const failures = _report.failureCount() + _teardownFailures;

      if (failures == 0)
      {
        std::cerr << "WinUI track table probe: all scenarios passed\n";
        exitCode = 0;
      }
      else
      {
        std::cerr << std::format("WinUI track table probe: {} check(s) failed\n", failures);
        exitCode = 1;
      }

      if (_window && !_windowClosed)
      {
        _window.Close();
      }

      Exit();
    }

    bool _contentLoaded = false;
    bool _finishing = false;
    bool _windowClosed = false;
    std::size_t _teardownFailures = 0;

    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> _appProvider{nullptr};
    Window _window{nullptr};
    Grid _root{nullptr};
    Border _hostBorder{nullptr};
    FrameworkElement _titleBarSlot{nullptr};
    DispatcherTimer _timeout{nullptr};
    Window::Closed_revoker _windowClosedRevoker{};
    FrameworkElement::LayoutUpdated_revoker _layoutRevoker{};

    ScenarioReport _report;
    std::optional<TempRoot> _tempRoot;

    std::vector<TrackId> _seededTrackIds;
    std::vector<TrackId> _sortedSeedIds;
    TrackId _firstTrack{};
    TrackId _secondTrack{};
    TrackId _anchorTrack{};
    TrackId _revealTrackId{};
    Item _headerItem{nullptr};

    std::unique_ptr<ao::rt::AppRuntime> _runtimePtr;
    ao::winui::DispatcherQueueExecutor* _executorPtr = nullptr;
    std::unique_ptr<ao::uimodel::TrackColumnLayouts> _columnLayoutsPtr;
    std::unique_ptr<ao::winui::TrackListController> _trackListPtr;
    ao::async::Subscription _selectionSub;
    std::vector<std::vector<TrackId>> _selectionPublications;
    std::vector<std::string> _statusMessages;

    std::unique_ptr<ao::uimodel::TrackPresentationCatalog> _presentationCatalogPtr;
    std::unique_ptr<ao::uimodel::ListPresentations> _listPresentationsPtr;
    std::unique_ptr<ao::winui::ThemeCoordinator> _themeCoordinatorPtr;
    std::unique_ptr<ao::uimodel::PlaybackActions> _playbackActionsPtr;
    ao::async::Signal<ao::winui::ShellState> _shellStateChanged;
    ao::async::Signal<ao::winui::layout::WindowActivityState> _windowActivityChanged;
    std::unique_ptr<ao::winui::layout::LayoutComponent> _presentationButtonPtr;
    std::unique_ptr<ao::winui::layout::LayoutComponent> _transportButtonPtr;
    Controls::Button _presentationButton{nullptr};
    Controls::Button _transportButton{nullptr};
    ao::uimodel::ListPresentations::Snapshot _presentationsBefore;

    FrameworkElement::Loaded_revoker _navigationLoadedRevoker{};
    Controls::NavigationView _navigation{nullptr};
    FrameworkElement _navigationContent{nullptr};
    Button _navigationPresentationButton{nullptr};
    std::size_t _navigationWidthIndex = 0;
    bool _navigationSortChecked = false;
    bool _navigationLightChecked = false;
    std::optional<float> _optNavigationChromeBandTop;

    Item _liveButtonIdentity{nullptr};
    std::uint32_t _liveRepeatGlyphValue = 0;
    std::uint32_t _liveRepeatOneGlyphValue = 0;

    std::unique_ptr<ao::uimodel::LayoutSchema> _schemaPtr;
    ao::winui::layout::ActionRegistry _actions;
    std::unique_ptr<ao::winui::layout::ComponentRegistry> _registryPtr;
    ResourceDictionary _resources{nullptr};
    std::unique_ptr<ao::winui::layout::LayoutHost> _hostPtr;
    std::shared_ptr<ao::uimodel::ShellGenerationGate> _gatePtr;
    std::shared_ptr<ao::winui::layout::FocusedDetail> _focusedDetailPtr;

    ao::winui::ShellState _shellState{};
    ao::winui::layout::WindowActivityState _windowActivity{.visible = true, .minimized = false};
    std::string _statusMessage{};

    ListView _listView{nullptr};
    ItemsStackPanel _rowsPanel{nullptr};
  };
} // namespace

int main()
{
  try
  {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Application::Start([](auto&&) { std::ignore = winrt::make<WinUiTrackTableProbeApplication>(); });
    return exitCode;
  }
  catch (winrt::hresult_error const& error)
  {
    std::cerr << std::format("WinUI track table probe startup failed: {} (HRESULT {:#x})\n",
                             winrt::to_string(error.message()),
                             static_cast<std::uint32_t>(error.code().value));
    return 2;
  }
  catch (std::exception const& error)
  {
    std::cerr << std::format("WinUI track table probe failed: {}\n", error.what());
    return 3;
  }
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Desktop-only native-property evidence, not a Catch case or popup-pixel test.
// Usage: ao_winui_quality_tooltip_probe.exe STATE_ROOT AUDIO_FIXTURE
// Both paths must be absolute and outside the checkout. STATE_ROOT must not
// exist; its parent must exist. AUDIO_FIXTURE is a read-only FLAC fixture (for
// example basic_metadata.flac). Keep the new state directory as run evidence.

#include "XamlMetaDataProvider.h"
#include "app/DispatcherQueueExecutor.h"
#include "lib/audio/NullBackend.h"
#include "platform/StringResources.h"
#include "playback/AudioPipelineToolTip.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/NodeFormat.h>
#include <ao/audio/flow/Graph.h>
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
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryDurability.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>
#include <ao/utility/Path.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
  using namespace winrt::Microsoft::UI::Xaml;
  using Automation::AutomationProperties;
  using Controls::ToolTipService;

  struct ProbeInputs final
  {
    std::filesystem::path stateRoot;
    std::filesystem::path fixture;
  };

  int exitCode = 1;

  // No render worker, endpoint, route graph, or invented delivery evidence.
  class QualityProbeBackend final : public ao::audio::NullBackend
  {
  public:
    ao::audio::BackendId backendId() const override { return ao::audio::BackendId{"winui-quality-probe"}; }
  };

  class QualityProbeProvider final : public ao::audio::BackendProvider
  {
  public:
    void shutdown() noexcept override { _closed = true; }

    Status status() const override
    {
      return {.descriptor = {.id = ao::audio::BackendId{"winui-quality-probe"},
                             .supportedProfiles = {{.id = ao::audio::ProfileId{ao::audio::kProfileShared}}}},
              .devices = _closed ? std::vector<ao::audio::Device>{} : std::vector<ao::audio::Device>{device()}};
    }

    ao::utility::ScopedRegistration subscribeDevices(OnDevicesChangedCallback callback) override
    {
      // Registration and retirement are owner-thread-only. The initial callback
      // may destroy this facade: nothing accesses it after calling outward.
      if (!_closed)
      {
        auto const devices = status().devices;
        callback(devices);
      }

      return {};
    }

    std::unique_ptr<ao::audio::Backend> createBackend(ao::audio::Device const& /*device*/,
                                                      ao::audio::ProfileId const& /*profile*/) override
    {
      return std::make_unique<QualityProbeBackend>();
    }

    ao::utility::ScopedRegistration subscribeGraph(std::string_view /*routeAnchor*/,
                                                   OnGraphChangedCallback /*callback*/) override
    {
      return {};
    }

    static ao::audio::Device device()
    {
      return {.id = ao::audio::DeviceId{"discard"},
              .displayName = "Quality probe discard sink",
              .isDefault = true,
              .backendId = ao::audio::BackendId{"winui-quality-probe"}};
    }

  private:
    bool _closed = false;
  };

  // C++/WinRT derives its allocation wrapper from this implementation.
  class WinUiQualityToolTipProbeApplication
    : public ApplicationT<WinUiQualityToolTipProbeApplication, Markup::IXamlMetadataProvider>
  {
  public:
    explicit WinUiQualityToolTipProbeApplication(ProbeInputs inputs)
      : _inputs{std::move(inputs)}
    {
      UnhandledException(
        [](auto const&, UnhandledExceptionEventArgs const& arguments)
        {
          AO_FATAL("Quality tooltip probe XAML exception {:#x}: {}",
                   static_cast<std::uint32_t>(arguments.Exception().value),
                   winrt::to_string(arguments.Message()));
        });
      Application::LoadComponent(*this, winrt::Windows::Foundation::Uri{L"ms-appx:///App.xaml"});
    }

    void OnLaunched(LaunchActivatedEventArgs const& /*arguments*/)
    {
      _window = Window{};
      _window.Title(L"Aobus quality tooltip property probe");
      _root = Controls::Grid{};
      _anchor = Controls::Button{};
      _anchor.Content(winrt::box_value(L"Quality tooltip probe"));
      _anchor.HorizontalAlignment(HorizontalAlignment::Center);
      _anchor.VerticalAlignment(VerticalAlignment::Center);
      _loadedRevoker = _anchor.Loaded(winrt::auto_revoke, [this](auto const&, auto const&) { enqueueDrive(); });
      _sizeRevoker = _root.SizeChanged(winrt::auto_revoke, [this](auto const&, auto const&) { enqueueDrive(); });
      _closedRevoker = _window.Closed(winrt::auto_revoke,
                                      [this](auto const&, auto const&)
                                      {
                                        _windowClosed = true;

                                        if (!_finishing)
                                        {
                                          fail("window closed before the property observations completed");
                                        }
                                      });
      _root.Children().Append(_anchor);
      _window.Content(_root);
      _window.AppWindow().Resize({.Width = 960, .Height = 640});
      // Intrinsic native-probe failure boundary, not readiness or an outer
      // process watchdog. A missing decoder publication or size event fails.
      _deadline = _window.DispatcherQueue().CreateTimer();
      _deadline.Interval(std::chrono::seconds{90});
      _deadline.IsRepeating(false);
      _deadline.Tick([this](auto const&, auto const&) { fail("90-second dispatcher deadline expired"); });
      _deadline.Start();
      _window.Activate();
    }

    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type)
    {
      return AppProvider()->GetXamlType(type);
    }

    Markup::IXamlType GetXamlType(winrt::hstring const& fullName) { return AppProvider()->GetXamlType(fullName); }

    winrt::com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return AppProvider()->GetXmlnsDefinitions(); }

  private:
    enum class Phase
    {
      NormalLayout,
      Ready,
      Assessment,
      ConstrainedLayout,
    };

    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> AppProvider()
    {
      if (!_appProvider)
      {
        _appProvider = winrt::make_self<winrt::Aobus::implementation::XamlMetaDataProvider>();
      }

      return _appProvider;
    }

    void check(bool condition, std::string_view observation)
    {
      if (!condition)
      {
        _failed = true;
        std::cerr << std::format("FAIL [quality tooltip, phase {}]: {}\n", static_cast<int>(_phase), observation);
      }
    }

    void fail(std::string_view observation)
    {
      check(false, observation);
      requestFinish();
    }

    void enqueueDrive()
    {
      if (_finishing || _driveQueued)
      {
        return;
      }

      _driveQueued = true;
      auto const admitted = _window.DispatcherQueue().TryEnqueue(
        [this]
        {
          _driveQueued = false;

          if (_finishing)
          {
            return;
          }

          try
          {
            drive();
          }
          catch (winrt::hresult_error const& error)
          {
            fail(std::format("native step exception {:#x}: {}",
                             static_cast<std::uint32_t>(error.code().value),
                             winrt::to_string(error.message())));
          }
          catch (std::exception const& error)
          {
            fail(std::format("probe step exception: {}", error.what()));
          }
        });
      AO_INVARIANT(admitted, "Quality tooltip dispatcher rejected a native observation step");
    }

    ao::Result<> seedLibrary()
    {
      auto libraryRes = ao::library::MusicLibrary::open(
        _inputs.fixture.parent_path(), _inputs.stateRoot / "database", {.flushCommits = false});

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
        [this](ao::library::LibraryWrite& write) -> ao::Result<>
        {
          auto const uri = ao::utility::pathToGenericUtf8(_inputs.fixture.filename());
          auto builder = ao::library::TrackBuilder::makeEmpty();
          builder.metadata().title("Decoded quality fixture");
          builder.property().uri(uri);
          auto createRes = write.tracks().create(builder, ao::library::FileManifestBuilder::makeEmpty());

          if (!createRes)
          {
            return std::unexpected{createRes.error()};
          }

          _trackId = *createRes;
          return {};
        });

      if (!applyRes)
      {
        return std::unexpected{applyRes.error()};
      }

      return transaction.commit();
    }

    void setup()
    {
      // Exclusive creation refuses reused state. No copying, cleanup, settings
      // discovery, or write into the fixture's parent directory is performed.
      if (!std::filesystem::create_directory(_inputs.stateRoot))
      {
        fail("STATE_ROOT already exists; use a fresh evidence directory");
        return;
      }

      auto seedRes = seedLibrary();

      if (!seedRes)
      {
        fail(std::format("public library writer failed: {}", seedRes.error().message));
        return;
      }

      auto catalogRes = ao::i18n::MessageCatalog::create("en");

      if (!catalogRes)
      {
        fail(std::format("English catalog failed: {}", catalogRes.error().message));
        return;
      }

      auto executorPtr = std::make_unique<ao::winui::DispatcherQueueExecutor>(_window.DispatcherQueue());
      auto* const executor = executorPtr.get();
      auto runtimeRes = ao::rt::AppRuntime::create({
        .executorPtr = std::move(executorPtr),
        .musicRoot = _inputs.fixture.parent_path(),
        .databasePath = _inputs.stateRoot / "database",
        .musicLibraryDurability = ao::rt::LibraryDurability::Unflushed,
        .workspaceConfigStorePtr = std::make_unique<ao::rt::ConfigStore>(_inputs.stateRoot / "workspace.yaml"),
      });

      if (!runtimeRes)
      {
        fail(std::format("AppRuntime creation failed: {}", runtimeRes.error().message));
        return;
      }

      _runtimePtr = std::make_unique<ao::rt::AppRuntime>(std::move(*runtimeRes));
      // This borrow is published only after the executor's owner is placed.
      _executor = executor;
      _runtimePtr->sealPlaybackSessionPersistenceWrites();
      auto& playback = _runtimePtr->playback();
      _toolTipPtr = std::make_unique<ao::winui::AudioPipelineToolTip>(
        ao::winui::AudioPipelineToolTipConfig{.anchor = _anchor, .textCatalog = *catalogRes}, playback);
      _referencePtr =
        std::make_unique<ao::uimodel::NowPlayingViewModel>(playback,
                                                           *catalogRes,
                                                           [this](ao::uimodel::NowPlayingViewState const& state)
                                                           {
                                                             _reference = state;
                                                             enqueueDrive();
                                                           });
      observeNative(false);
      check(_reference.audioPipeline.quality.assessments.empty(), "initial reference has no pipeline assessments");
      check(_reference.audioPipeline.plainTextFallback.empty(), "initial reference fallback is empty");

      if (_failed)
      {
        requestFinish();
        return;
      }

      _snapshotSub = playback.events().onSnapshot(
        [this](ao::rt::PlaybackSnapshot const& snapshot)
        {
          if (snapshot.transport.nowPlaying.trackId == _trackId && hasDecodedSource(snapshot.transport.quality))
          {
            _assessmentPublished = true;
          }

          enqueueDrive();
        });
      auto viewRes = _runtimePtr->workspace().navigate({.target = ao::rt::GlobalViewKind::AllTracks});

      if (!viewRes)
      {
        fail(std::format("public AllTracks navigation failed: {}", viewRes.error().message));
        return;
      }

      _viewId = *viewRes;
      _phase = Phase::Ready;
      _runtimePtr->addAudioProvider(std::make_unique<QualityProbeProvider>());
      auto const device = QualityProbeProvider::device();
      playback.commands().setOutputDevice(device.backendId, device.id, ao::audio::ProfileId{ao::audio::kProfileShared});
      enqueueDrive();
    }

    static bool hasDecodedSource(ao::rt::QualityState const& quality)
    {
      return std::ranges::any_of(
        quality.assessments,
        [](auto const& assessment)
        {
          if (assessment.nodeType != ao::audio::flow::NodeType::Source || !assessment.optFormat)
          {
            return false;
          }

          auto const format = ao::audio::signalFormat(*assessment.optFormat);
          return format.sampleRate > 0 && format.channels > 0;
        });
    }

    void observeNative(bool available)
    {
      auto const toolTip = ToolTipService::GetToolTip(_anchor).try_as<Controls::ToolTip>();
      check(static_cast<bool>(toolTip), "anchor owns the shipping native ToolTip");

      if (!toolTip)
      {
        return;
      }

      auto const text = toolTip.Content().try_as<Controls::TextBlock>();
      check(static_cast<bool>(text), "ToolTip content is a native TextBlock");

      if (!text)
      {
        return;
      }

      auto const& pipeline = _reference.audioPipeline;
      auto const expected = winrt::to_hstring(pipeline.plainTextFallback);
      check(available == (!pipeline.quality.assessments.empty() && !pipeline.plainTextFallback.empty()),
            "expected availability agrees with the independent real VM state");
      check(_anchor.IsLoaded(), "anchor remains in the loaded owned window");
      check(text.Text() == expected, "native TextBlock text equals the independent real VM fallback");
      check(AutomationProperties::GetHelpText(_anchor) == (available ? expected : winrt::hstring{}),
            "native HelpText equals the available reference fallback");
      check(toolTip.IsEnabled() == available, "native ToolTip availability follows actual assessments");
      check(text.MaxWidth() == 360.0, "shipping TextBlock MaxWidth remains 360");
      check(text.TextWrapping() == TextWrapping::Wrap, "shipping TextBlock keeps Wrap at both window sizes");
      std::cerr << std::format("quality tooltip phase {}: window={}x{}, root-width={}, enabled={}, max-width={}, "
                               "wrap={}, text=[{}], HelpText=[{}]\n",
                               static_cast<int>(_phase),
                               _window.AppWindow().Size().Width,
                               _window.AppWindow().Size().Height,
                               _root.ActualWidth(),
                               toolTip.IsEnabled(),
                               text.MaxWidth(),
                               text.TextWrapping() == TextWrapping::Wrap,
                               winrt::to_string(text.Text()),
                               winrt::to_string(AutomationProperties::GetHelpText(_anchor)));
    }

    void drive()
    {
      switch (_phase)
      {
        case Phase::NormalLayout:
          if (_anchor.IsLoaded() && _root.ActualWidth() > 0 && _window.AppWindow().Size().Width == 960)
          {
            _normalWidth = _root.ActualWidth();
            setup();
          }
          break;
        case Phase::Ready:
          if (_runtimePtr->playback().snapshot().transport.ready)
          {
            _phase = Phase::Assessment;
            auto startRes = _runtimePtr->playback().commands().startFromView(_viewId, _trackId);

            if (!startRes)
            {
              fail(std::format("public startFromView failed: {}", startRes.error().message));
            }
          }
          break;
        case Phase::Assessment:
          if (_assessmentPublished && hasDecodedSource(_reference.audioPipeline.quality))
          {
            auto const& pipeline = _reference.audioPipeline;
            auto const& snapshot = _runtimePtr->playback().snapshot();
            check(snapshot.transport.nowPlaying.trackId == _trackId, "decoded publication belongs to the seeded track");
            check(pipeline.quality == snapshot.transport.quality, "independent VM observed the committed quality");
            check(pipeline.quality.channelConfidence == ao::audio::ChannelConfidence::Unknown,
                  "real decoder publication retains Unknown channel confidence");
            auto catalogRes = ao::i18n::MessageCatalog::create("en");

            if (!catalogRes)
            {
              fail(std::format("note catalog failed: {}", catalogRes.error().message));
              return;
            }

            auto const presentation = ao::uimodel::AudioQualityFormatter{*catalogRes}.presentation(pipeline.quality);
            check(!presentation.channelNote.empty(), "real Unknown assessment produces a nonempty channel note");
            check(presentation.channelNote ==
                    ao::i18n::requiredText(*catalogRes, ao::i18n::MessageId::AudioQualityUnknownChannelLayout),
                  "published note is the catalog's Unknown-layout note");
            check(pipeline.plainTextFallback.ends_with("\n" + presentation.channelNote),
                  "actual VM fallback includes the published channel note");

            for (auto const& assessment : pipeline.quality.assessments)
            {
              if (assessment.optFormat)
              {
                check(ao::audio::signalFormat(*assessment.optFormat).speakerMask == 0,
                      "actual no-producer formats retain mask zero");
              }
            }

            observeNative(true); // The required empty-to-published native update.
            _publishedFallback = pipeline.plainTextFallback;
            _phase = Phase::ConstrainedLayout;
            _window.AppWindow().Resize({.Width = 360, .Height = 480});
          }
          break;
        case Phase::ConstrainedLayout:
          if (_window.AppWindow().Size().Width == 360 && _root.ActualWidth() > 0 && _root.ActualWidth() < _normalWidth)
          {
            check(_reference.audioPipeline.plainTextFallback == _publishedFallback,
                  "real quality fallback is invariant under owner-window resizing");
            observeNative(true);
            _observationsComplete = !_failed;
            requestFinish();
          }
          break;
      }

      if (_failed)
      {
        requestFinish();
      }
    }

    void observeDetached()
    {
      auto const attached = static_cast<bool>(ToolTipService::GetToolTip(_anchor));
      auto const helpText = AutomationProperties::GetHelpText(_anchor);
      check(!attached, "tooltip destructor clears the native anchor attachment");
      check(helpText.empty(), "tooltip destructor clears native HelpText");
      std::cerr << std::format(
        "quality tooltip cleanup: attached={}, HelpText=[{}]\n", attached, winrt::to_string(helpText));
    }

    void requestFinish()
    {
      if (_finishing)
      {
        return;
      }

      _finishing = true;
      // Never close or destroy a subscriber/runtime on its observer stack, nor
      // inside DispatcherQueueExecutor's admitted callback/drain stack.
      auto const admitted = _window.DispatcherQueue().TryEnqueue([this] { finish(); });
      AO_INVARIANT(admitted, "Quality tooltip dispatcher rejected deferred teardown");
    }

    void finish()
    {
      try
      {
        _deadline.Stop();
        _loadedRevoker.revoke();
        _sizeRevoker.revoke();
        bool const hadToolTip = _toolTipPtr != nullptr;
        _toolTipPtr.reset();

        if (hadToolTip)
        {
          observeDetached();
        }

        if (_executor != nullptr)
        {
          _executor->beginClosing();
          _runtimePtr->shutdown();      // Cancels preparation and joins all producers.
          _executor->completeClosing(); // Settle admitted callbacks with consumers alive.
        }

        if (hadToolTip)
        {
          observeDetached(); // Final queued publications cannot reattach a retired consumer.
        }

        _snapshotSub.reset();
        _referencePtr.reset();
        _executor = nullptr;
        _runtimePtr.reset();
        _closedRevoker.revoke();
        exitCode = !_failed && _observationsComplete ? 0 : 1;
        std::cerr << std::format(
          "WinUI quality tooltip probe: {} (native properties only)\n", exitCode == 0 ? "passed" : "failed");

        if (!_windowClosed)
        {
          _window.Close();
        }

        Exit();
      }
      catch (winrt::hresult_error const& error)
      {
        AO_FATAL("Quality tooltip teardown exception {:#x}: {}",
                 static_cast<std::uint32_t>(error.code().value),
                 winrt::to_string(error.message()));
      }
      catch (std::exception const& error)
      {
        AO_FATAL("Quality tooltip teardown exception: {}", error.what());
      }
    }

    ProbeInputs _inputs;
    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> _appProvider{nullptr};
    Window _window{nullptr};
    Controls::Grid _root{nullptr};
    Controls::Button _anchor{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _deadline{nullptr};
    FrameworkElement::Loaded_revoker _loadedRevoker{};
    FrameworkElement::SizeChanged_revoker _sizeRevoker{};
    Window::Closed_revoker _closedRevoker{};
    // Explicit finish() closes/drains before destruction; declaration order also
    // keeps UI and runtime alive longer than their subscribing consumers.
    std::unique_ptr<ao::rt::AppRuntime> _runtimePtr;
    ao::winui::DispatcherQueueExecutor* _executor = nullptr; // Owned by _runtimePtr.
    std::unique_ptr<ao::winui::AudioPipelineToolTip> _toolTipPtr;
    std::unique_ptr<ao::uimodel::NowPlayingViewModel> _referencePtr;
    ao::async::Subscription _snapshotSub;
    ao::uimodel::NowPlayingViewState _reference{};
    ao::TrackId _trackId = ao::kInvalidTrackId;
    ao::rt::ViewId _viewId = ao::rt::kInvalidViewId;
    Phase _phase = Phase::NormalLayout;
    std::string _publishedFallback;
    double _normalWidth = 0;
    bool _assessmentPublished = false;
    bool _observationsComplete = false;
    bool _driveQueued = false;
    bool _failed = false;
    bool _finishing = false;
    bool _windowClosed = false;
  };
} // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc != 3)
    {
      std::cerr << "Usage: ao_winui_quality_tooltip_probe.exe STATE_ROOT AUDIO_FIXTURE\n";
      return 2;
    }

    auto inputs = ProbeInputs{ao::utility::pathFromUtf8(argv[1]), ao::utility::pathFromUtf8(argv[2])};

    if (!inputs.stateRoot.is_absolute() || !inputs.fixture.is_absolute() ||
        !std::filesystem::is_regular_file(inputs.fixture) || inputs.fixture.extension() != L".flac")
    {
      std::cerr << "Quality tooltip probe requires absolute paths and an existing read-only FLAC fixture\n";
      return 2;
    }

    inputs.fixture = std::filesystem::canonical(inputs.fixture);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    auto languageRes = ao::winui::configureResourceLanguage("en");

    if (!languageRes)
    {
      std::cerr << std::format("Quality tooltip MRT setup failed: {}\n", languageRes.error().message);
      return 2;
    }

    Application::Start([inputs = std::move(inputs)](auto const&)
                       { std::ignore = winrt::make<WinUiQualityToolTipProbeApplication>(inputs); });
    ao::winui::resetResourceLanguage();
    return exitCode;
  }
  catch (winrt::hresult_error const& error)
  {
    std::cerr << std::format("Quality tooltip probe startup failed {:#x}: {}\n",
                             static_cast<std::uint32_t>(error.code().value),
                             winrt::to_string(error.message()));
    return 2;
  }
  catch (std::exception const& error)
  {
    std::cerr << std::format("Quality tooltip probe startup failed: {}\n", error.what());
    return 2;
  }
}

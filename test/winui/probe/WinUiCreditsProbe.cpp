// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Loaded native Credits controls, driven through XAML properties, automation
// Invoke and native focus navigation, followed by the shipping detail control
// and Properties coordinator over a disposable runtime. No physical input or
// MainWindow modal-admission coverage is claimed.
#include "XamlMetaDataProvider.h"
#include "app/DispatcherQueueExecutor.h"
#include "platform/ResourceLanguage.h"
#include "track/TrackCreditsEditorControl.h"
#include "track/TrackDetailControl.h"
#include "track/TrackPropertiesCoordinator.h"
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
  using namespace winrt::Microsoft::UI::Xaml;
  using namespace winrt::Microsoft::UI::Xaml::Controls;
  using ao::i18n::MessageId;
  using ao::library::Credit;
  using ao::library::CreditKind;

  class TempRoot final
  {
  public:
    TempRoot()
      : _path{std::filesystem::temp_directory_path() /
              std::format("aobus-winui-credits-probe-{}", std::chrono::steady_clock::now().time_since_epoch().count())}
    {
      std::filesystem::create_directories(_path / "database");
    }
    TempRoot(TempRoot const&) = delete;
    TempRoot& operator=(TempRoot const&) = delete;
    TempRoot(TempRoot&&) = delete;
    TempRoot& operator=(TempRoot&&) = delete;

    ~TempRoot()
    {
      auto error = std::error_code{};
      std::filesystem::remove_all(_path, error);
    }
    std::filesystem::path const& path() const { return _path; }

  private:
    std::filesystem::path _path;
  };

  template<typename T>
  void collect(DependencyObject const& root, std::vector<T>& controls)
  {
    if (!root)
    {
      return;
    }

    if (auto control = root.try_as<T>(); control)
    {
      controls.push_back(control);
    }

    for (std::int32_t index = 0; index < Media::VisualTreeHelper::GetChildrenCount(root); ++index)
    {
      collect(Media::VisualTreeHelper::GetChild(root, index), controls);
    }
  }

  ao::uimodel::TrackCreditSections emptySections()
  {
    auto sections = ao::uimodel::TrackCreditSections{};

    for (auto& section : sections)
    {
      section.optValue = std::vector<Credit>{};
    }

    return sections;
  }

  // C++/WinRT's allocation wrapper derives from this application implementation.
  class AobusCreditsProbeApplication : public ApplicationT<AobusCreditsProbeApplication, Markup::IXamlMetadataProvider>
  {
  public:
    explicit AobusCreditsProbeApplication(std::int32_t& exitCode)
      : _exitCode{exitCode}
    {
      UnhandledException(
        [this](auto const&, UnhandledExceptionEventArgs const& args)
        {
          args.Handled(true);
          fail(winrt::to_string(args.Message()));
        });
      Application::LoadComponent(*this, winrt::Windows::Foundation::Uri{L"ms-appx:///App.xaml"});
    }

    void OnLaunched(LaunchActivatedEventArgs const& /*arguments*/)
    {
      auto catalogRes = ao::i18n::MessageCatalog::create("en");
      auto seedRes = seedLibrary();
      auto languageRes = ao::winui::configureResourceLanguage("en");

      if (!catalogRes)
      {
        fail(catalogRes.error().message);
        return;
      }

      if (!seedRes)
      {
        fail(seedRes.error().message);
        return;
      }

      if (!languageRes)
      {
        fail(languageRes.error().message);
        return;
      }

      _optCatalog = std::move(*catalogRes);
      _window = Window{};
      _window.Title(L"Aobus scoped Credits workflow probe");
      auto executorPtr = std::make_unique<ao::winui::DispatcherQueueExecutor>(_window.DispatcherQueue());
      auto* executor = executorPtr.get();
      auto runtimeRes = ao::rt::AppRuntime::create({
        .executorPtr = std::move(executorPtr),
        .musicRoot = _temp.path(),
        .databasePath = _temp.path() / "database",
        .workspaceConfigStorePtr = std::make_unique<ao::rt::ConfigStore>(_temp.path() / "workspace.yaml"),
      });

      if (!runtimeRes)
      {
        fail(runtimeRes.error().message);
        return;
      }

      _runtimePtr = std::make_unique<ao::rt::AppRuntime>(std::move(*runtimeRes));
      _runtimeExecutor = executor;
      _baselineRevision = _runtimePtr->library().snapshot().revision();
      _publicationSub = _runtimePtr->library().changes().onChanged(
        [this](ao::rt::LibraryChangeSet const& change)
        {
          _publications.push_back(change);
          next();
        });
      _scroll = ScrollViewer{};
      _scroll.Width(320.0);
      _scroll.Height(560.0);
      _scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
      _scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
      _window.Content(_scroll);
      _layoutRevoker = _scroll.LayoutUpdated(winrt::auto_revoke, [this](auto const&, auto const&) { next(); });
      _timeout = _window.DispatcherQueue().CreateTimer();
      _timeout.Interval(std::chrono::seconds{90});
      _timeout.IsRepeating(false);
      _timeout.Tick([this](auto const&, auto const&) { fail("dispatcher deadline exceeded"); });
      _timeout.Start();
      _dropDownPoll = _window.DispatcherQueue().CreateTimer();
      _dropDownPoll.Interval(std::chrono::milliseconds{50});
      _dropDownPoll.Tick([this](auto const&, auto const&) { next(); });
      planScenarios();
      planFeedbackScenarios();
      planWorkflowScenarios();
      planBatchPropertiesScenarios();
      planFailedPropertiesSave();
      planFailedPropertiesReload();
      _window.Activate();
      next();
    }

    // The IXamlMetadataProvider boundary requires these exact names for C++/WinRT consume/produce dispatch.
    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type)
    {
      return provider()->GetXamlType(type);
    }

    Markup::IXamlType GetXamlType(winrt::hstring const& name) { return provider()->GetXamlType(name); }

    winrt::com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return provider()->GetXmlnsDefinitions(); }

  private:
    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> provider()
    {
      if (!_providerPtr)
      {
        _providerPtr = winrt::make_self<winrt::Aobus::implementation::XamlMetaDataProvider>();
      }

      return _providerPtr;
    }

    void check(bool const condition, std::string_view const observation)
    {
      if (!condition)
      {
        ++_failures;
        std::cerr << std::format("FAIL [step {}]: {}\n", _step, observation);
      }
    }

    void fail(std::string_view const reason)
    {
      check(false, reason);
      dumpState();
      finish();
    }

    void next()
    {
      if (_finished)
      {
        return;
      }

      if (_step == _steps.size())
      {
        finish();
        return;
      }

      if (_queued)
      {
        return;
      }

      _queued = true;

      if (!_window.DispatcherQueue().TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
                                                [this]
                                                {
                                                  _queued = false;

                                                  if (_finished)
                                                  {
                                                    return;
                                                  }

                                                  try
                                                  {
                                                    // A dispatcher turn alone does not imply that XAML has loaded a new
                                                    // template or delivered AutoSuggestBox's deferred TextChanged
                                                    // event.
                                                    if (!isNativeReady())
                                                    {
                                                      return;
                                                    }

                                                    _ready = {};
                                                    _textObservations.clear();
                                                    _textRevokers.clear();
                                                    _fieldTextRevokers.clear();
                                                    _suggestionRevokers.clear();
                                                    std::cout << std::format("STEP {} ready\n", _step + 1)
                                                              << std::flush;
                                                    _steps[_step++]();
                                                    next();
                                                  }
                                                  catch (winrt::hresult_error const& error)
                                                  {
                                                    fail(winrt::to_string(error.message()));
                                                  }
                                                }))
      {
        fail("dispatcher rejected a live probe step");
      }
    }

    bool isNativeReady()
    {
      if (!_scroll || !_scroll.IsLoaded())
      {
        return false;
      }

      if (_controlPtr)
      {
        if (!_controlPtr->element().IsLoaded() || _controlPtr->element().ActualWidth() <= 0.0)
        {
          return false;
        }

        for (auto const& input : controls<AutoSuggestBox>())
        {
          auto textBoxes = std::vector<TextBox>{};
          collect(input, textBoxes);

          if (!input.IsLoaded() || textBoxes.empty() || !textBoxes[0].IsLoaded())
          {
            return false;
          }
        }
      }

      if (_detailPtr && !_expectDialog)
      {
        auto const content = _scroll.Content().try_as<FrameworkElement>();

        if (!content || !content.IsLoaded() || content.ActualWidth() <= 0.0)
        {
          return false;
        }

        // Show Empty recreates actions synchronously, but XAML loads their native controls later.
        for (auto const& command : controls<Button>())
        {
          if (!command.IsLoaded())
          {
            return false;
          }
        }
      }

      if (_expectDialog && (!_workflowDialog || (!_dialogClosed && !_workflowDialog.IsLoaded())))
      {
        return false;
      }

      if (_workflowDialog && !_dialogClosed)
      {
        auto content = _workflowDialog.Content().try_as<FrameworkElement>();

        if (!content || !content.IsLoaded() || content.ActualWidth() <= 0.0)
        {
          return false;
        }

        for (auto const& command : controls<Button>())
        {
          if (!command.IsLoaded())
          {
            return false;
          }
        }

        for (auto const& input : controls<AutoSuggestBox>())
        {
          auto textBoxes = std::vector<TextBox>{};
          collect(input, textBoxes);

          if (!input.IsLoaded() || textBoxes.empty() || !textBoxes[0].IsLoaded())
          {
            return false;
          }
        }
      }

      if (_ready && !_ready())
      {
        return false;
      }

      return std::ranges::all_of(_textObservations, [](bool const observed) { return observed; });
    }

    void setText(AutoSuggestBox const& input, winrt::hstring const& text)
    {
      auto const index = _textObservations.size();
      _textObservations.push_back(false);
      _textRevokers.push_back(input.TextChanged(
        winrt::auto_revoke,
        [this, index, text](AutoSuggestBox const& sender, AutoSuggestBoxTextChangedEventArgs const& args)
        {
          std::cout << std::format("TEXT step={} reason={} text={} loaded={}\n",
                                   _step,
                                   static_cast<std::int32_t>(args.Reason()),
                                   winrt::to_string(sender.Text()),
                                   sender.IsLoaded())
                    << std::flush;
          _textObservations[index] = sender.Text() == text;
          next();
        }));
      input.Text(text);
    }

    void dumpState()
    {
      std::cout << std::format("STATE step={} nativeReady={} editing={} canEdit={} canCommit={} focusedRow={}\n",
                               _step,
                               isNativeReady(),
                               _model.isEditing(),
                               _model.canEdit(),
                               _model.canCommit(),
                               _model.focusedRow() ? std::to_string(*_model.focusedRow()) : "none");

      for (std::size_t index = 0; index < _textObservations.size(); ++index)
      {
        std::cout << std::format(
          "TEXT_OBSERVATION index={} delivered={}\n", index, static_cast<bool>(_textObservations[index]));
      }

      for (auto const& entry : _model.entries())
      {
        std::cout << std::format(
          "MODEL name={} kind={} role={}\n", entry.name, static_cast<std::int32_t>(entry.kind), entry.role);
      }

      if (_controlPtr)
      {
        for (auto const& input : controls<AutoSuggestBox>())
        {
          std::cout << std::format("INPUT header={} text={} loaded={} focus={} width={}\n",
                                   winrt::to_string(winrt::unbox_value<winrt::hstring>(input.Header())),
                                   winrt::to_string(input.Text()),
                                   input.IsLoaded(),
                                   isFocusedWithin(input),
                                   input.ActualWidth());
        }
      }

      if (_kindSelector)
      {
        auto const item = _kindSelector.ContainerFromIndex(3).try_as<ComboBoxItem>();
        std::cout << std::format("KIND_SELECTOR open={} selected={} item={} itemLoaded={} popups={}\n",
                                 _kindSelector.IsDropDownOpen(),
                                 _kindSelector.SelectedIndex(),
                                 static_cast<bool>(item),
                                 item && item.IsLoaded(),
                                 Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(_scroll.XamlRoot()).Size());
      }

      std::cout << std::format("WORKFLOW active={} closed={} publications={} externalCompleted={}\n",
                               _coordinatorPtr && _coordinatorPtr->isActive(),
                               _dialogClosed,
                               _publications.size(),
                               _externalCompleted);
      std::cout << std::flush;
    }

    void finish()
    {
      if (_finished)
      {
        return;
      }

      _finished = true;
      _exitCode = _failures == 0 && _step == _steps.size() ? 0 : 1;
      std::cerr << std::format("WinUI Credits probe: {} steps, {} failures, exit {}\n", _step, _failures, _exitCode);

      if (_timeout)
      {
        _timeout.Stop();
      }

      if (_dropDownPoll)
      {
        _dropDownPoll.Stop();
      }

      _layoutRevoker.revoke();
      _textRevokers.clear();
      _fieldTextRevokers.clear();
      _suggestionRevokers.clear();
      _dialogLayoutRevoker.revoke();
      _dialogClosedRevoker.revoke();
      _publicationSub.reset();
      _workflowTasks.cancelAll();
      _coordinatorPtr.reset();
      _detailPtr.reset();
      _controlPtr.reset();

      if (_runtimeExecutor != nullptr)
      {
        _runtimeExecutor->beginClosing();
        _runtimePtr->shutdown();
        _runtimeExecutor->completeClosing();
        _runtimeExecutor = nullptr;
      }

      _runtimePtr.reset();
      ao::winui::resetResourceLanguage();

      if (_window)
      {
        _window.Close();
      }

      Exit();
    }

    template<typename T>
    std::vector<T> controls() const
    {
      auto found = std::vector<T>{};
      auto root = DependencyObject{nullptr};

      if (_workflowDialog)
      {
        root = _workflowDialog.as<DependencyObject>();
      }
      else if (_controlPtr)
      {
        root = _controlPtr->element().as<DependencyObject>();
      }
      else
      {
        root = _scroll.as<DependencyObject>();
      }

      collect(root, found);

      if (_workflowDialog)
      {
        // ContentDialog may reparent its template into a Popup. Include its
        // authored content and native popup tree, retaining each object once.
        auto popupControls = std::vector<T>{};
        collect(_workflowDialog.Content().as<DependencyObject>(), popupControls);

        for (auto const& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(_scroll.XamlRoot()))
        {
          collect(popup.Child(), popupControls);
        }

        for (auto const& control : popupControls)
        {
          if (!std::ranges::contains(found, control))
          {
            found.push_back(control);
          }
        }
      }

      if (!_controlPtr)
      {
        std::erase_if(found, [](T const& control) { return !isVisible(control); });
      }

      return found;
    }

    std::vector<AutoSuggestBox> inputs(MessageId const id) const
    {
      auto found = std::vector<AutoSuggestBox>{};
      auto const label = winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, id));

      for (auto const& input : controls<AutoSuggestBox>())
      {
        if (input.Header() && winrt::unbox_value<winrt::hstring>(input.Header()) == label)
        {
          found.push_back(input);
        }
      }

      return found;
    }

    Button button(MessageId const id, std::size_t const occurrence = 0)
    {
      auto const label = winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, id));
      std::size_t index = 0;

      for (auto const& candidate : controls<Button>())
      {
        if (auto text = candidate.Content().try_as<TextBlock>(); text && text.Text() == label && index++ == occurrence)
        {
          return candidate;
        }
      }

      throw winrt::hresult_invalid_argument{L"Required semantic Credits button is not present"};
    }

    void invoke(MessageId const id, std::size_t const occurrence = 0)
    {
      auto target = button(id, occurrence);

      if (!target)
      {
        return;
      }

      check(target.IsEnabled(), "invoked button is enabled");
      auto peer = Automation::Peers::ButtonAutomationPeer{target};
      peer.GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
    }

    static winrt::hstring automationName(Button const& target)
    {
      return Automation::Peers::ButtonAutomationPeer{target}.GetName();
    }

    static Automation::Provider::IExpandCollapseProvider expandCollapse(ComboBox const& target)
    {
      return Automation::Peers::ComboBoxAutomationPeer{target}
        .GetPattern(Automation::Peers::PatternInterface::ExpandCollapse)
        .as<Automation::Provider::IExpandCollapseProvider>();
    }

    // Repeated row commands keep their short visible text but expose distinct row names.
    void checkRowCommandNames(std::size_t const rowCount)
    {
      using ao::uimodel::TrackCreditRowAction;
      auto const actions = std::array{std::pair{MessageId::TrackCreditMoveUp, TrackCreditRowAction::MoveUp},
                                      std::pair{MessageId::TrackCreditMoveDown, TrackCreditRowAction::MoveDown},
                                      std::pair{MessageId::TrackCreditDelete, TrackCreditRowAction::Delete}};

      for (auto const& [id, action] : actions)
      {
        for (std::size_t row = 0; row < rowCount; ++row)
        {
          check(automationName(button(id, row)) ==
                  winrt::to_hstring(ao::uimodel::formatTrackCreditRowActionLabel(*_optCatalog, action, row)),
                "row command exposes its row-specific automation name");
        }
      }
    }

    void show(ao::uimodel::TrackCreditSections const& sections, std::bitset<ao::library::kCreditKindCount> const scope)
    {
      _controlPtr.reset();
      _optAccepted.reset();

      if (auto beginRes = _model.begin(sections, scope); !beginRes)
      {
        fail(beginRes.error().message);
        return;
      }

      _controlPtr = std::make_unique<ao::winui::TrackCreditsEditorControl>(
        _model,
        _runtimePtr->completion(),
        *_optCatalog,
        [this]
        {
          auto patchRes = _model.buildCommitPatch();
          check(patchRes.has_value(), "native Save admits only valid rows");

          if (patchRes)
          {
            _optAccepted = std::move(*patchRes);
            _model.cancel();
          }
        },
        [this]
        {
          ++_cancelled;
          _model.cancel();
        });
      _scroll.Content(_controlPtr->element());
    }

    bool isFocusedWithin(DependencyObject const& ancestor) const
    {
      auto focused = Input::FocusManager::GetFocusedElement(_scroll.XamlRoot()).try_as<DependencyObject>();

      while (focused)
      {
        if (focused == ancestor)
        {
          return true;
        }

        focused = Media::VisualTreeHelper::GetParent(focused);
      }

      return false;
    }

    static bool isVisible(DependencyObject control)
    {
      while (control)
      {
        if (auto element = control.try_as<UIElement>(); element && element.Visibility() != Visibility::Visible)
        {
          return false;
        }

        control = Media::VisualTreeHelper::GetParent(control);
      }

      return true;
    }

    ao::Result<> seedLibrary()
    {
      auto libraryRes = ao::library::MusicLibrary::open(_temp.path(), _temp.path() / "database");

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
          auto const credits = expectedCredits("Conductor", "Anne");
          auto builder = ao::library::TrackBuilder::makeEmpty();
          builder.metadata().title("Original title").album("First album").year(1997).credits(credits);
          builder.customMetadata().add("Café", "Original partial value");
          builder.property().uri("offline-probe.flac");
          auto createdRes = write.tracks().create(builder, ao::library::FileManifestBuilder::makeEmpty());

          if (!createdRes)
          {
            return std::unexpected{createdRes.error()};
          }

          _trackId = *createdRes;
          auto emptyBuilder = ao::library::TrackBuilder::makeEmpty();
          emptyBuilder.metadata().album("Second album").year(1997);
          emptyBuilder.property().uri("empty-probe.flac");
          auto emptyRes = write.tracks().create(emptyBuilder, ao::library::FileManifestBuilder::makeEmpty());

          if (!emptyRes)
          {
            return std::unexpected{emptyRes.error()};
          }

          _emptyTrackId = *emptyRes;
          auto outside = ao::library::TrackBuilder::makeEmpty();
          auto additionalSoloists = std::vector<Credit>{};

          for (std::int32_t index = 0; index < 13; ++index)
          {
            additionalSoloists.push_back({.name = std::format("Anz {}", index), .kind = CreditKind::Soloist});
          }

          outside.metadata().album("Outside album").credits(additionalSoloists);
          outside.property().uri("outside-probe.flac");
          auto outsideRes = write.tracks().create(outside, ao::library::FileManifestBuilder::makeEmpty());

          if (!outsideRes)
          {
            return std::unexpected{outsideRes.error()};
          }

          _outsideTrackId = *outsideRes;
          return {};
        });

      if (!applyRes)
      {
        return std::unexpected{applyRes.error()};
      }

      if (auto commitRes = transaction.commit(); !commitRes)
      {
        return std::unexpected{commitRes.error()};
      }

      return {};
    }

    static std::vector<Credit> expectedCredits(std::string conductor, std::string soloist)
    {
      return {{.name = std::move(conductor), .kind = CreditKind::Conductor, .role = "baton"},
              {.name = std::move(soloist), .kind = CreditKind::Soloist, .role = "violin"},
              {.name = "Accompanist", .kind = CreditKind::Performer, .role = "piano"},
              {.name = "Accompanist", .kind = CreditKind::Performer, .role = "piano"}};
    }

    void checkStored(std::string const& title, std::vector<Credit> const& credits, std::uint64_t revisionDelta)
    {
      auto snapshot = _runtimePtr->library().snapshot();
      check(snapshot.trackField(_trackId, ao::rt::TrackField::Title) == ao::rt::TrackFieldRawValue{title},
            "database title matches the accepted parent draft");
      check(snapshot.trackCredits(_trackId) == std::optional{credits},
            "database credits preserve exact order, roles, and unselected duplicates");
      check(snapshot.revision() == _baselineRevision + revisionDelta, "exact number of durable commits");
      check(_publications.size() == revisionDelta, "one publication per composed commit, none for child staging");

      for (std::size_t index = 0; index < _publications.size(); ++index)
      {
        check(_publications[index].libraryRevision == _baselineRevision + index + 1 &&
                _publications[index].tracksMutated == std::vector{_trackId},
              "publication identifies the captured target and committed revision");
      }
    }

    void invokeButton(Button const& target)
    {
      if (!target || !target.IsLoaded() || !target.IsEnabled() || !isVisible(target))
      {
        std::cerr << std::format("INVOKE_BUTTON step={} present={} loaded={} enabled={} visible={}\n",
                                 _step,
                                 static_cast<bool>(target),
                                 target && target.IsLoaded(),
                                 target && target.IsEnabled(),
                                 target && isVisible(target));
        throw winrt::hresult_invalid_argument{L"Workflow button is not loaded, visible and enabled"};
      }

      auto peer = Automation::Peers::ButtonAutomationPeer{target};
      peer.GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
    }

    void invokeScope(CreditKind kind)
    {
      auto const label =
        winrt::to_hstring(ao::i18n::requiredFormat(*_optCatalog,
                                                   MessageId::TrackCreditsScope,
                                                   {{"scope", ao::uimodel::trackCreditKindLabel(*_optCatalog, kind)}}));

      for (auto const& candidate : controls<Button>())
      {
        if (auto text = candidate.Content().try_as<TextBlock>(); text && text.Text() == label)
        {
          invokeButton(candidate);
          _ready = [this] { return inputs(MessageId::TrackCreditName).size() == 1; };
          return;
        }
      }

      throw winrt::hresult_invalid_argument{L"Shipping category entrypoint is missing"};
    }

    void invokeDialogButton(std::wstring_view name)
    {
      for (auto const& candidate : controls<Button>())
      {
        if (candidate.Name() == name)
        {
          invokeButton(candidate);

          if (name == L"CloseButton")
          {
            _ready = [this]
            {
              auto const label =
                winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, MessageId::WinUiTrackPropertiesDiscard));

              return std::ranges::any_of(controls<Button>(),
                                         [&label](Button const& command)
                                         {
                                           auto const text = command.Content().try_as<TextBlock>();
                                           return text && text.Text() == label;
                                         });
            };
          }

          return;
        }
      }

      throw winrt::hresult_invalid_argument{L"Native ContentDialog command is missing"};
    }

    AutoSuggestBox singleCreditName()
    {
      auto names = inputs(MessageId::TrackCreditName);

      if (names.size() != 1)
      {
        throw winrt::hresult_invalid_argument{L"Scoped coordinator child must contain exactly one name"};
      }

      return names.front();
    }

    TextBox textField(ao::rt::TrackField field) const
    {
      auto const label = winrt::to_hstring(ao::uimodel::trackFieldLabel(*_optCatalog, field));

      for (auto const& input : controls<TextBox>())
      {
        if (input.Header() && winrt::unbox_value<winrt::hstring>(input.Header()) == label)
        {
          return input;
        }
      }

      throw winrt::hresult_invalid_argument{L"Properties text input is missing"};
    }

    TextBox titleInput() const { return textField(ao::rt::TrackField::Title); }

    void setTitle(winrt::hstring const& text) { setText(titleInput(), text); }

    void setText(TextBox const& input, winrt::hstring const& text)
    {
      auto const index = _textObservations.size();
      _textObservations.push_back(false);
      _fieldTextRevokers.push_back(input.TextChanged(winrt::auto_revoke,
                                                     [this, input, index, text](auto const&, auto const&)
                                                     {
                                                       _textObservations[index] = input.Text() == text;
                                                       next();
                                                     }));
      input.Text(text);
    }

    void showDetail(std::vector<ao::TrackId> ids = {}, bool const probeActions = false)
    {
      _controlPtr.reset();
      _detailPtr.reset();
      _probeDetailActions = probeActions;
      _detailCallbackIds.clear();
      _optDetailCallbackScope.reset();

      if (!probeActions)
      {
        ids = {_trackId};
      }

      _detailExpectedIds = ids;
      auto root = StackPanel{};
      auto rows = StackPanel{};
      _detailMetadataHeader = Button{};
      _detailShowEmpty = Button{};
      root.Children().Append(_detailMetadataHeader);
      root.Children().Append(_detailShowEmpty);
      root.Children().Append(rows);
      _scroll.Content(root);
      _detailPtr = std::make_unique<ao::winui::TrackDetailControl>(
        ao::winui::TrackDetailControlConfig{
          .fieldScroll = _scroll,
          .detailContent = root,
          .metadataHeaderButton = _detailMetadataHeader,
          .metadataRows = rows,
          .showEmptyButton = _detailShowEmpty,
          .textCatalog = *_optCatalog,
          .editCredits =
            [this](std::vector<ao::TrackId> ids, std::bitset<ao::library::kCreditKindCount> scope)
          {
            if (_probeDetailActions)
            {
              _detailCallbackIds = std::move(ids);
              _optDetailCallbackScope = scope;
              return;
            }

            check(ids == std::vector{_trackId}, "shipping detail action captures the projected target");
            check(scope == ao::uimodel::trackCreditScope(CreditKind::Soloist),
                  "shipping detail action routes the exact requested category");
            openProperties(std::move(ids), scope);
          },
        },
        _runtimePtr->workspace().detailProjection(ao::rt::ExplicitSelectionTarget{std::move(ids)}));
      _ready = [rows] { return rows.IsLoaded() && rows.ActualWidth() > 0.0; };
    }

    void openProperties(std::vector<ao::TrackId> ids,
                        std::optional<std::bitset<ao::library::kCreditKindCount>> optScope = std::nullopt)
    {
      _coordinatorPtr =
        std::make_unique<ao::winui::TrackPropertiesCoordinator>(ao::winui::TrackPropertiesCoordinatorConfig{
          .xamlRoot = _scroll.XamlRoot(),
          .asyncRuntime = _runtimePtr->async(),
          .library = _runtimePtr->library(),
          .workspace = _runtimePtr->workspace(),
          .completion = _runtimePtr->completion(),
          .textCatalog = *_optCatalog,
          .trackIds = std::move(ids),
          .optCreditsScope = optScope,
        });

      if (auto presentRes = _coordinatorPtr->present(); !presentRes)
      {
        throw winrt::hresult_invalid_argument{winrt::to_hstring(presentRes.error().message)};
      }

      _workflowDialog = _coordinatorPtr->element();
      _dialogLayoutRevoker =
        _workflowDialog.LayoutUpdated(winrt::auto_revoke, [this](auto const&, auto const&) { next(); });
      _dialogClosedRevoker = _workflowDialog.Closed(winrt::auto_revoke,
                                                    [this](auto const&, auto const&)
                                                    {
                                                      _dialogClosed = true;
                                                      next();
                                                    });
      _expectDialog = true;
    }

    void releaseClosedDialog()
    {
      check(_dialogClosed && !_coordinatorPtr->isActive(), "native Closed retires the shipping coordinator");
      _dialogLayoutRevoker.revoke();
      _dialogClosedRevoker.revoke();
      _workflowDialog = nullptr;
      _coordinatorPtr.reset();
      _expectDialog = false;
      _dialogClosed = false;
    }

    template<typename Reply, typename IsAccepted>
    static ao::async::Task<void> observeCommandAsync(AobusCreditsProbeApplication* owner,
                                                     ao::async::Task<ao::Result<Reply>> submission,
                                                     IsAccepted isAccepted,
                                                     std::stop_token stopToken)
    {
      auto res = co_await std::move(submission);
      co_await owner->_runtimePtr->async().resumeOnCallbackExecutorAsync(stopToken);

      if (!owner->_finished)
      {
        owner->_externalAccepted = res && isAccepted(*res);
        owner->_externalCompleted = true;
        owner->next();
      }
    }

    template<typename Reply, typename IsAccepted>
    void observeCommand(ao::async::Task<ao::Result<Reply>> submission, IsAccepted isAccepted, std::string_view context)
    {
      _externalCompleted = false;
      _runtimePtr->async().spawnWithLifetime(
        _workflowTasks,
        [this, submission = std::move(submission), isAccepted = std::move(isAccepted)](
          std::stop_token stopToken) mutable
        { return observeCommandAsync(this, std::move(submission), std::move(isAccepted), stopToken); },
        context);
      _ready = [this] { return _externalCompleted; };
    }

    void mutateExternally()
    {
      auto bindingRes = _runtimePtr->library().bindTrackTargets(std::vector{_trackId});

      if (!bindingRes)
      {
        throw winrt::hresult_invalid_argument{winrt::to_hstring(bindingRes.error().message)};
      }

      auto submission =
        _runtimePtr->library().commands().updateMetadataAsync(std::move(*bindingRes), {.optTitle = "External title"});
      observeCommand(
        std::move(submission),
        [](auto const& reply) { return reply.status == ao::rt::AuthoringStatus::Applied; },
        "Credits probe external revision");
    }

    void deleteCapturedTrack(ao::TrackId id)
    {
      observeCommand(
        _runtimePtr->library().commands().deleteTrackAsync(id),
        [id](auto const& reply) { return reply.trackId == id; },
        "Credits probe captured track deletion");
    }

    void previewTitleClear(bool const expectRecorded)
    {
      // Only a normally blank title still needs a persisted clear marker.
      auto const clearRecord = [](ao::TrackId id)
      {
        return ao::rt::TrackChangeRecord{
          .trackId = id,
          .fields = {{.field = "title",
                      .oldValue = "",
                      .newValue = "",
                      .optReason = ao::rt::TrackFieldChangeReason::ExplicitClearRecorded}},
        };
      };
      auto expected = expectRecorded ? std::vector{clearRecord(_outsideTrackId)}
                                     : std::vector{clearRecord(_emptyTrackId), clearRecord(_outsideTrackId)};
      observeCommand(
        _runtimePtr->library().commands().previewUpdateMetadataAsync(
          {_emptyTrackId, _outsideTrackId}, {.optTitle = ""}),
        [expected = std::move(expected)](auto const& reply) { return reply.changes == expected; },
        "Credits probe blank Title clear preview");
    }

    // The all-kind action and the four performance kinds, or with Show Empty every kind.
    static constexpr std::size_t kPerformanceActions = 5;
    static constexpr std::size_t kEveryKindActions = 1 + ao::library::kCreditKindCount;

    void checkDetailActions(std::size_t const expected)
    {
      checkCreditActions(
        expected, "Show Empty binds the whole Credits section and the actions of kinds after Performer");
    }

    void checkCreditActions(std::size_t const expected, std::string_view const observation)
    {
      std::size_t count = 0;

      for (auto const& command : controls<Button>())
      {
        auto text = command.Content().try_as<TextBlock>();

        if (!text)
        {
          continue;
        }

        auto const allLabel = winrt::to_hstring(
          ao::i18n::requiredFormat(*_optCatalog,
                                   MessageId::TrackCreditsScope,
                                   {{"scope", ao::i18n::requiredText(*_optCatalog, MessageId::TrackCreditsAllKinds)}}));
        bool isCreditAction = text.Text() == allLabel;

        for (std::size_t kindIndex = 0; kindIndex < ao::library::kCreditKindCount; ++kindIndex)
        {
          auto const kind = static_cast<CreditKind>(kindIndex);
          auto const label = winrt::to_hstring(
            ao::i18n::requiredFormat(*_optCatalog,
                                     MessageId::TrackCreditsScope,
                                     {{"scope", ao::uimodel::trackCreditKindLabel(*_optCatalog, kind)}}));
          isCreditAction = isCreditAction || text.Text() == label;
        }

        if (isCreditAction)
        {
          ++count;
          check(command.IsEnabled(), "visible shipping Credits action is enabled");
        }
      }

      check(count == expected, observation);
    }

    void invokeDetailScope(std::bitset<ao::library::kCreditKindCount> const scope)
    {
      auto const label = winrt::to_hstring(
        ao::i18n::requiredFormat(*_optCatalog,
                                 MessageId::TrackCreditsScope,
                                 {{"scope", ao::uimodel::trackCreditScopeLabel(*_optCatalog, scope)}}));

      for (auto const& command : controls<Button>())
      {
        if (auto text = command.Content().try_as<TextBlock>(); text && text.Text() == label)
        {
          invokeButton(command);
          check(_detailCallbackIds == _detailExpectedIds && _optDetailCallbackScope == scope,
                "shipping detail callback carries exact projected targets and requested scope");
          return;
        }
      }

      throw winrt::hresult_invalid_argument{L"Required shipping Credits scope action is missing"};
    }

    void requireNativeRows(std::size_t const count)
    {
      if (inputs(MessageId::TrackCreditName).size() != count || inputs(MessageId::TrackCreditRole).size() != count)
      {
        throw winrt::hresult_invalid_argument{L"Feedback scenario must retain the expected complete native rows"};
      }
    }

    void editNativeName(winrt::hstring const& text)
    {
      requireNativeRows(3);
      auto names = inputs(MessageId::TrackCreditName);
      auto const& input = names.back();
      auto textBoxes = std::vector<TextBox>{};
      collect(input, textBoxes);

      if (textBoxes.empty())
      {
        throw winrt::hresult_invalid_argument{L"Loaded name template lacks its native TextBox"};
      }

      if (_typingBox)
      {
        check(_typingBox == textBoxes.front(), "consecutive value edits retain the native typing control");
      }

      _typingBox = textBoxes.front();
      check(_typingBox.Focus(FocusState::Programmatic), "native typing field accepts focus");
      _userInputObserved = false;
      auto const index = _textObservations.size();
      _textObservations.push_back(false);
      _textRevokers.push_back(input.TextChanged(
        winrt::auto_revoke,
        [this, index, text](AutoSuggestBox const& sender, AutoSuggestBoxTextChangedEventArgs const& args)
        {
          _textObservations[index] = sender.Text() == text;
          _userInputObserved = args.Reason() == AutoSuggestionBoxTextChangeReason::UserInput;
          next();
        }));
      _typingBox.Text(text);
      _typingBox.Select(static_cast<std::int32_t>(text.size()), 0);
    }

    void chooseNativeSuggestion(AutoSuggestBox const& input, winrt::hstring const& text)
    {
      _suggestionChosenObserved = false;
      _suggestionRevokers.push_back(input.SuggestionChosen(
        winrt::auto_revoke,
        [this, text](AutoSuggestBox const&, AutoSuggestBoxSuggestionChosenEventArgs const& args)
        {
          _suggestionChosenObserved = winrt::unbox_value<winrt::hstring>(args.SelectedItem()) == text;
          next();
        }));
      auto lists = std::vector<ListView>{};
      collect(input, lists);

      for (auto const& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(_scroll.XamlRoot()))
      {
        collect(popup.Child(), lists);
      }

      for (auto const& list : lists)
      {
        for (auto const& item : list.Items())
        {
          if (auto value = item.try_as<winrt::Windows::Foundation::IPropertyValue>();
              value && value.Type() == winrt::Windows::Foundation::PropertyType::String && value.GetString() == text)
          {
            // Use the real suggestion list selection path, not a fabricated SuggestionChosen event.
            setTextObservation(input, text);
            list.SelectedItem(item);
            _ready = [this] { return _suggestionChosenObserved; };
            return;
          }
        }
      }

      throw winrt::hresult_invalid_argument{L"Loaded native suggestion list lacks the expected vocabulary item"};
    }

    void setTextObservation(AutoSuggestBox const& input, winrt::hstring const& text)
    {
      auto const index = _textObservations.size();
      _textObservations.push_back(false);
      _textRevokers.push_back(
        input.TextChanged(winrt::auto_revoke,
                          [this, index, text](AutoSuggestBox const& sender, AutoSuggestBoxTextChangedEventArgs const&)
                          {
                            _textObservations[index] = sender.Text() == text;
                            next();
                          }));
    }

    void observeUnchangedHydration()
    {
      requireNativeRows(3);
      auto const names = inputs(MessageId::TrackCreditName);
      auto const roles = inputs(MessageId::TrackCreditRole);
      auto const name = names[0].Text();
      auto const role = roles[0].Text();
      check(_model.focusedRow() == 2 && isFocusedWithin(names[2]),
            "retirement or refresh preserves logical and native focus before hydration fixture changes");
      // Force observed current-source events; do not assume constructor hydration delivery.
      // Disabling protects transient values even when XAML delivers them synchronously.
      _controlPtr->setEnabled(false);
      names[0].Text(L"Hydration name prelude");
      roles[0].Text(L"Hydration role prelude");
      _controlPtr->setEnabled(true);
      check(names[2].Focus(FocusState::Programmatic), "hydration fixture restores the intended native focus");
      setText(names[0], name);
      setText(roles[0], role);
    }

    void planFeedbackScenarios()
    {
      _steps.emplace_back(
        [this]
        {
          auto sections = emptySections();
          sections[0].optValue = std::vector<Credit>{{.name = "Atlas", .kind = CreditKind::Conductor, .role = "baton"}};
          sections[2].optValue = std::vector<Credit>{{.name = "Beta", .kind = CreditKind::Soloist, .role = "violin"},
                                                     {.name = "Gamma", .kind = CreditKind::Soloist, .role = "cello"}};
          show(sections, ao::uimodel::allTrackCreditKinds());
          _model.focusRow(2);
        });
      _steps.emplace_back([this] { observeUnchangedHydration(); });
      _steps.emplace_back(
        [this]
        {
          requireNativeRows(3);
          check(_model.focusedRow() == 2 && isFocusedWithin(inputs(MessageId::TrackCreditName)[2]),
                "observed unchanged hydration preserves logical and actual last-row focus");
          _retiredName = inputs(MessageId::TrackCreditName)[0];
          _retiredRole = inputs(MessageId::TrackCreditRole)[0];
          _retiredKind = controls<ComboBox>()[0];
          _retiredDelete = button(MessageId::TrackCreditDelete, 0);
          _retiredKind.SelectedIndex(2);
        });
      _steps.emplace_back(
        [this]
        {
          requireNativeRows(3);
          check(
            _model.entries() == std::vector<Credit>{{.name = "Beta", .kind = CreditKind::Soloist, .role = "violin"},
                                                    {.name = "Gamma", .kind = CreditKind::Soloist, .role = "cello"},
                                                    {.name = "Atlas", .kind = CreditKind::Soloist, .role = "baton"}},
            "multirow reclassification retains values and appends to the destination segment");
          check(_model.focusedRow() == 2 && isFocusedWithin(inputs(MessageId::TrackCreditName)[2]),
                "reclassification hydration keeps logical and native focus on the moved row");
          check(
            _retiredName != inputs(MessageId::TrackCreditName)[0], "structural refresh retires the old native source");
          // Detached revoked controls need not deliver TextChanged; never wait for those events.
          _retiredName.Text(L"Retired name");
          _retiredRole.Text(L"Retired role");
          _retiredKind.SelectedIndex(3);
          auto peer = Automation::Peers::ButtonAutomationPeer{_retiredDelete};
          peer.GetPattern(Automation::Peers::PatternInterface::Invoke)
            .as<Automation::Provider::IInvokeProvider>()
            .Invoke();
          observeUnchangedHydration();
        });
      _steps.emplace_back(
        [this]
        {
          requireNativeRows(3);
          check(_model.entries().size() == 3 && _model.entries()[0].name == "Beta" &&
                  _model.entries()[2] == Credit{.name = "Atlas", .kind = CreditKind::Soloist, .role = "baton"},
                "retained retired text/kind/delete sources cannot edit replacement rows");
          check(_model.focusedRow() == 2 && isFocusedWithin(inputs(MessageId::TrackCreditName)[2]),
                "retired callbacks cannot claim logical or actual focus");
          _controlPtr->refresh();
        });
      _steps.emplace_back([this] { observeUnchangedHydration(); });
      _steps.emplace_back(
        [this]
        {
          requireNativeRows(3);
          check(_model.focusedRow() == 2 && isFocusedWithin(inputs(MessageId::TrackCreditName)[2]),
                "explicit multirow refresh cannot claim focus through unchanged deferred text");
          editNativeName(L"An");
        });
      _steps.emplace_back(
        [this]
        {
          check(_userInputObserved && _model.entries()[2].name == "An" && _model.entries()[2].role == "baton",
                "native TextBox input updates only the focused name");
          auto input = inputs(MessageId::TrackCreditName)[2];
          check(input.IsSuggestionListOpen() && input.ItemsSource(), "UserInput retains category completion");
          bool hasAnne = false;
          std::size_t candidates = 0;

          if (input.ItemsSource())
          {
            for (auto const& item :
                 input.ItemsSource()
                   .as<winrt::Windows::Foundation::Collections::IIterable<winrt::Windows::Foundation::IInspectable>>())
            {
              ++candidates;
              hasAnne = hasAnne || winrt::unbox_value<winrt::hstring>(item) == L"Anne";
            }
          }

          check(candidates == 12, "native Credits suggestions retain the twelve-item window");
          check(hasAnne, "soloist completion uses the reclassified row kind and seeded native runtime vocabulary");
          check(_typingBox.SelectionStart() == 2 && _typingBox.SelectionLength() == 0 && isFocusedWithin(input),
                "deferred input/completion retains actual focus and caret");
          editNativeName(L"Ann");
        });
      _steps.emplace_back(
        [this]
        {
          check(_userInputObserved && _model.focusedRow() == 2 && _model.entries()[2].name == "Ann" &&
                  _typingBox.SelectionStart() == 3 && _typingBox.SelectionLength() == 0 &&
                  isFocusedWithin(inputs(MessageId::TrackCreditName)[2]),
                "consecutive native edits preserve logical focus, actual focus, value and caret");
          chooseNativeSuggestion(inputs(MessageId::TrackCreditName)[2], L"Anne");
        });
      _steps.emplace_back(
        [this]
        {
          check(_suggestionChosenObserved && _model.focusedRow() == 2 &&
                  _model.entries()[2] == Credit{.name = "Anne", .kind = CreditKind::Soloist, .role = "baton"} &&
                  isFocusedWithin(inputs(MessageId::TrackCreditName)[2]),
                "real SuggestionChosen updates the value without losing role, logical or actual focus");
          setText(inputs(MessageId::TrackCreditName)[2], L"Anne edited");
          setText(inputs(MessageId::TrackCreditRole)[2], L"chosen role");
        });
      _steps.emplace_back(
        [this]
        {
          check(
            _model.entries()[2] == Credit{.name = "Anne edited", .kind = CreditKind::Soloist, .role = "chosen role"},
            "genuine programmatic value edits still mutate names and roles");
          invoke(MessageId::TrackCreditDelete, 2);
        });
      _steps.emplace_back(
        [this]
        {
          requireNativeRows(2);
          check(_model.entries().size() == 2 && _model.focusedRow() == 1 &&
                  isFocusedWithin(inputs(MessageId::TrackCreditName)[1]) &&
                  inputs(MessageId::TrackCreditRole)[1].Text() == L"cello",
                "Delete rehydrates remaining rows with correct logical and native successor focus");
          invoke(MessageId::TrackCreditsCancel);
          _typingBox = nullptr;
          _retiredName = nullptr;
          _retiredRole = nullptr;
          _retiredKind = nullptr;
          _retiredDelete = nullptr;
          showDetail({_emptyTrackId}, true);
        });
      _steps.emplace_back(
        [this]
        {
          checkDetailActions(0);
          invokeButton(_detailShowEmpty);
        });
      _steps.emplace_back(
        [this]
        {
          checkDetailActions(kEveryKindActions);
          invokeDetailScope(ao::uimodel::allTrackCreditKinds());

          for (std::size_t kindIndex = 0; kindIndex < ao::library::kCreditKindCount; ++kindIndex)
          {
            invokeDetailScope(ao::uimodel::trackCreditScope(static_cast<CreditKind>(kindIndex)));
          }

          invokeButton(_detailShowEmpty);
        });
      _steps.emplace_back(
        [this]
        {
          checkDetailActions(0);
          showDetail({_trackId}, true);
        });
      _steps.emplace_back(
        [this]
        {
          checkDetailActions(kPerformanceActions);
          invokeDetailScope(ao::uimodel::trackCreditScope(CreditKind::Ensemble));
          invokeButton(_detailShowEmpty);
        });
      _steps.emplace_back(
        [this]
        {
          checkDetailActions(kEveryKindActions);
          invokeButton(_detailMetadataHeader);
        });
      _steps.emplace_back(
        [this]
        {
          checkDetailActions(0);
          invokeButton(_detailMetadataHeader);
        });
      _steps.emplace_back(
        [this]
        {
          checkDetailActions(kEveryKindActions);
          showDetail({_trackId, _emptyTrackId}, true);
        });
      _steps.emplace_back(
        [this]
        {
          // A fresh detail starts with Show Empty off, so only the performance kinds have actions.
          checkDetailActions(kPerformanceActions);
          bool hasMixed = false;

          for (auto const& text : controls<TextBlock>())
          {
            hasMixed = hasMixed || text.Text() == winrt::to_hstring(ao::i18n::requiredText(
                                                    *_optCatalog, MessageId::TrackMultipleValues));
          }

          check(
            hasMixed, "mixed selected Credits sections show the owning aggregate rather than a first-target preview");
          invokeDetailScope(ao::uimodel::trackCreditScope(CreditKind::Soloist));
          showDetail({}, true);
        });
      _steps.emplace_back(
        [this]
        {
          checkDetailActions(0);
          check(!_detailShowEmpty.IsEnabled(), "no selection disables the shipping Show Empty action");
          check(_detailCallbackIds.empty() && !_optDetailCallbackScope, "no selection emits no Credits callback");
        });
    }

    void planWorkflowScenarios()
    {
      auto const reloadQuestion =
        winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, MessageId::WinUiTrackPropertiesDiscardQuestion));
      _steps.emplace_back([this] { showDetail(); });
      _steps.emplace_back([this] { invokeScope(CreditKind::Soloist); });
      _steps.emplace_back(
        [this]
        {
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "active Credits child blocks composed Save");
          check(singleCreditName().Text() == L"Anne", "detail entrypoint opens captured scoped baseline");
          setText(singleCreditName(), L"Staged soloist");
          setTitle(L"Composed title");
        });
      _steps.emplace_back(
        [this]
        {
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "ordinary edits cannot bypass active child guard");
          invoke(MessageId::TrackCreditsCommit);
        });
      _steps.emplace_back(
        [this]
        {
          checkStored("Original title", expectedCredits("Conductor", "Anne"), 0);
          check(_workflowDialog.IsPrimaryButtonEnabled(), "accepted child enables one composed parent Save");
          checkCreditActions(
            kPerformanceActions, "Properties offers no one-kind action for an unused kind after Performer");
          invokeScope(CreditKind::Conductor);
        });
      _steps.emplace_back([this] { setText(singleCreditName(), L"Staged conductor"); });
      _steps.emplace_back([this] { invoke(MessageId::TrackCreditsCommit); });
      _steps.emplace_back([this] { invokeScope(CreditKind::Soloist); });
      _steps.emplace_back(
        [this]
        {
          check(singleCreditName().Text() == L"Staged soloist", "sequential scope reopening uses pending overlay");
          invoke(MessageId::TrackCreditsCancel);
        });
      _steps.emplace_back(
        [this]
        {
          checkStored("Original title", expectedCredits("Conductor", "Anne"), 0);
          invokeDialogButton(L"PrimaryButton");
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "parent submission disables repeated Save");
          _ready = [this] { return _dialogClosed; };
        });
      _steps.emplace_back(
        [this]
        {
          checkStored("Composed title", expectedCredits("Staged conductor", "Staged soloist"), 1);
          // Closing switches discovery back to detail; its refreshed actions need their own native readiness gate.
          releaseClosedDialog();
        });
      _steps.emplace_back([this] { invokeScope(CreditKind::Soloist); });
      _steps.emplace_back([this] { setText(singleCreditName(), L"Discarded soloist"); });
      _steps.emplace_back([this] { invoke(MessageId::TrackCreditsCommit); });
      _steps.emplace_back([this] { invokeDialogButton(L"CloseButton"); });
      _steps.emplace_back(
        [this]
        {
          check(_coordinatorPtr->isActive() && !_dialogClosed, "dirty native Close is cancelled for confirmation");
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "discard decision blocks Save");
          invoke(MessageId::WinUiTrackPropertiesKeepEditing);
        });
      _steps.emplace_back([this] { invokeScope(CreditKind::Soloist); });
      _steps.emplace_back(
        [this]
        {
          check(singleCreditName().Text() == L"Discarded soloist", "declining close preserves pending overlay");
          // Close with the child still active, not merely a staged parent patch.
          invokeDialogButton(L"CloseButton");
        });
      _steps.emplace_back(
        [this]
        {
          invoke(MessageId::WinUiTrackPropertiesDiscard);
          _ready = [this] { return _dialogClosed; };
        });
      _steps.emplace_back(
        [this]
        {
          checkStored("Composed title", expectedCredits("Staged conductor", "Staged soloist"), 1);
          releaseClosedDialog();
        });
      _steps.emplace_back([this] { invokeScope(CreditKind::Soloist); });
      _steps.emplace_back(
        [this]
        {
          setText(singleCreditName(), L"Stale draft");
          setTitle(L"Stale title draft");
          setText(textField(ao::rt::TrackField::Year), L"invalid stale year");
        });
      _steps.emplace_back([this] { mutateExternally(); });
      _steps.emplace_back(
        [this]
        {
          check(_externalAccepted, "external command completed with an applied revision");
          checkStored("External title", expectedCredits("Staged conductor", "Staged soloist"), 2);
          check(!singleCreditName().IsEnabled() && singleCreditName().Text() == L"Stale draft",
                "stale child is frozen without dropping its draft");
          check(!titleInput().IsEnabled() && titleInput().Text() == L"Stale title draft",
                "stale ordinary draft remains visible and disabled");
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "stale parent cannot submit");
          invokeDialogButton(L"CloseButton");
        });
      _steps.emplace_back(
        [this, reloadQuestion]
        {
          auto const stale =
            winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, MessageId::WinUiTrackPropertiesStale));
          check(std::ranges::any_of(
                  controls<TextBlock>(), [&stale](TextBlock const& label) { return label.Text() == stale; }),
                "discard detection preserves the stale diagnostic even with invalid scalar input");
          invoke(MessageId::WinUiTrackPropertiesKeepEditing);
          _ready = [this, reloadQuestion] { return _dialogClosed || !isTextVisible(reloadQuestion); };
        });
      _steps.emplace_back(
        [this, reloadQuestion]
        {
          if (_dialogClosed)
          {
            fail("Keep Editing must leave stale Properties open");
            return;
          }

          invoke(MessageId::WinUiTrackPropertiesReload);
          _ready = [this, reloadQuestion] { return isTextVisible(reloadQuestion); };
        });
      _steps.emplace_back(
        [this, reloadQuestion]
        {
          invokeDialogButton(L"CloseButton");
          _ready = [this, reloadQuestion] { return _dialogClosed || !isTextVisible(reloadQuestion); };
        });
      _steps.emplace_back(
        [this, reloadQuestion]
        {
          if (_dialogClosed)
          {
            fail("native Close must dismiss the reload confirmation, not Properties");
            return;
          }

          check(_coordinatorPtr->isActive() && !isTextVisible(reloadQuestion),
                "native Close cancels the reload confirmation");
          check(singleCreditName().Text() == L"Stale draft" && titleInput().Text() == L"Stale title draft" &&
                  textField(ao::rt::TrackField::Year).Text() == L"invalid stale year",
                "cancelling reload preserves child and ordinary drafts");
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "cancelling reload does not silently rebind");
          invoke(MessageId::WinUiTrackPropertiesReload);
          _ready = [this, reloadQuestion] { return isTextVisible(reloadQuestion); };
        });
      _steps.emplace_back(
        [this, reloadQuestion]
        {
          invoke(MessageId::WinUiTrackPropertiesDiscard);
          _ready = [this, reloadQuestion] { return _dialogClosed || !isTextVisible(reloadQuestion); };
        });
      _steps.emplace_back(
        [this]
        {
          if (_dialogClosed)
          {
            fail("confirming reload must rebind Properties in the open dialog");
            return;
          }

          check(inputs(MessageId::TrackCreditName).empty(), "confirmed reload discards active child");
          check(titleInput().Text() == L"External title" && titleInput().IsEnabled(),
                "confirmed reload replaces ordinary draft from committed revision");
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "reloaded unchanged form cannot submit");
          invokeScope(CreditKind::Soloist);
        });
      _steps.emplace_back(
        [this]
        {
          check(singleCreditName().Text() == L"Staged soloist", "reload discards stale scoped draft");
          setText(singleCreditName(), L"Reloaded soloist");
        });
      _steps.emplace_back([this] { invoke(MessageId::TrackCreditsCommit); });
      _steps.emplace_back(
        [this]
        {
          invokeDialogButton(L"PrimaryButton");
          _ready = [this] { return _dialogClosed; };
        });
      _steps.emplace_back(
        [this]
        {
          checkStored("External title", expectedCredits("Staged conductor", "Reloaded soloist"), 3);
          releaseClosedDialog();
          openProperties({_emptyTrackId});
          previewTitleClear(false);
        });
      _steps.emplace_back(
        [this]
        {
          check(_externalAccepted, "both blank titles still need an explicit clear before Save");
          check(titleInput().Text().empty() && !_workflowDialog.IsPrimaryButtonEnabled(),
                "an ordinary blank Title starts unchanged");
          auto const albumClear = fieldClearLabel(ao::rt::TrackField::Album, 1);
          check(std::ranges::none_of(controls<CheckBox>(),
                                     [&albumClear](CheckBox const& box)
                                     { return Automation::AutomationProperties::GetName(box) == albumClear; }),
                "single Properties does not offer the batch Album clear");
          toggleFieldClear(ao::rt::TrackField::Title, true, 1);
        });
      _steps.emplace_back(
        [this]
        {
          check(!titleInput().IsEnabled() && _workflowDialog.IsPrimaryButtonEnabled(),
                "explicit Clear on a blank Title disables its text editor and admits Save");
          toggleFieldClear(ao::rt::TrackField::Title, false, 1);
        });
      _steps.emplace_back(
        [this]
        {
          check(titleInput().IsEnabled() && titleInput().Text().empty() && !_workflowDialog.IsPrimaryButtonEnabled(),
                "unchecking Clear restores the ordinary blank draft");
          check(_runtimePtr->library().snapshot().revision() == _baselineRevision + 3 && _publications.size() == 3,
                "staging and withdrawing Title clear do not write");
          toggleFieldClear(ao::rt::TrackField::Title, true, 1);
        });
      _steps.emplace_back([this] { saveProperties(); });
      _steps.emplace_back(
        [this]
        {
          releaseClosedDialog();
          auto const snapshot = _runtimePtr->library().snapshot();
          check(_publications.size() == 4 && _publications.back().tracksMutated == std::vector{_emptyTrackId} &&
                  snapshot.revision() == _baselineRevision + 4,
                "Title clear commits once for the one bound track");
          check(snapshot.trackField(_emptyTrackId, ao::rt::TrackField::Album) ==
                    ao::rt::TrackFieldRawValue{std::string{"Second album"}} &&
                  snapshot.trackField(_emptyTrackId, ao::rt::TrackField::Year) ==
                    ao::rt::TrackFieldRawValue{std::uint16_t{1997}},
                "Title clear preserves unrelated fields on the bound track");
          previewTitleClear(true);
        });
      _steps.emplace_back(
        [this] { check(_externalAccepted, "only the unselected blank title still needs an explicit clear"); });
    }

    AutoSuggestBox suggestedField(ao::rt::TrackField field) const
    {
      auto const label = winrt::to_hstring(ao::uimodel::trackFieldLabel(*_optCatalog, field));

      for (auto const& input : controls<AutoSuggestBox>())
      {
        if (input.Header() && winrt::unbox_value<winrt::hstring>(input.Header()) == label)
        {
          return input;
        }
      }

      throw winrt::hresult_invalid_argument{L"Properties suggestion input is missing"};
    }

    AutoSuggestBox albumInput() const { return suggestedField(ao::rt::TrackField::Album); }

    AutoSuggestBox auxiliaryInput(MessageId id) const
    {
      auto const label = winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, id));

      for (auto const& input : controls<AutoSuggestBox>())
      {
        if (input.PlaceholderText() == label)
        {
          return input;
        }
      }

      throw winrt::hresult_invalid_argument{L"Missing Properties Add input"};
    }

    TextBox pendingCustomValue() const
    {
      auto const key = auxiliaryInput(MessageId::WinUiTrackPropertiesCustomKey);

      for (auto const& child : key.Parent().as<Grid>().Children())
      {
        if (auto input = child.try_as<TextBox>(); input)
        {
          return input;
        }
      }

      throw winrt::hresult_invalid_argument{L"Missing pending custom value input"};
    }

    bool isTextVisible(winrt::hstring const& expected) const
    {
      return std::ranges::any_of(
        controls<TextBlock>(), [&expected](TextBlock const& text) { return text.Text() == expected; });
    }

    winrt::hstring customKeyRejection(std::string_view key) const
    {
      return winrt::to_hstring(
        ao::i18n::requiredFormat(*_optCatalog, MessageId::WinUiTrackPropertiesInvalidValue, {{"detail", key}}));
    }

    bool isUnstagedInputHintVisible() const
    {
      return isTextVisible(
        winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, MessageId::TrackPropertiesUnstagedInputHint)));
    }

    void stageAuxiliaryInput(MessageId id, std::string_view rejectedKey = {})
    {
      auto input = auxiliaryInput(id);
      check(!_workflowDialog.IsPrimaryButtonEnabled(), "Save cannot drop an unadded input");

      for (auto const& child : input.Parent().as<Grid>().Children())
      {
        if (auto add = child.try_as<Button>(); add)
        {
          invokeButton(add);

          if (rejectedKey.empty())
          {
            _ready = [input] { return input.Text().empty(); };
          }
          else
          {
            auto const error = customKeyRejection(rejectedKey);
            _ready = [this, input, error] { return input.Text().empty() || isTextVisible(error); };
          }

          return;
        }
      }

      throw winrt::hresult_invalid_argument{L"Missing Add action for the input"};
    }

    winrt::hstring fieldClearLabel(ao::rt::TrackField selectedField, std::size_t count) const
    {
      auto const field = ao::uimodel::trackFieldLabel(*_optCatalog, selectedField);
      return winrt::to_hstring(ao::i18n::requiredFormat(
        *_optCatalog, MessageId::TrackClearFieldForTracks, {{"field", field}, {"count", count}}));
    }

    void toggleFieldClear(ao::rt::TrackField selectedField, bool checked, std::size_t count = 2)
    {
      auto const label = fieldClearLabel(selectedField, count);

      for (auto const& input : controls<CheckBox>())
      {
        if (auto text = input.Content().try_as<TextBlock>(); text && text.Text() == label && input.IsEnabled())
        {
          auto peer = Automation::Peers::CheckBoxAutomationPeer{input};
          check(peer.GetName() == label, "clear action exposes its field and count to automation");
          check(input.IsChecked().GetBoolean() != checked, "clear gesture starts in the expected state");
          peer.GetPattern(Automation::Peers::PatternInterface::Toggle)
            .as<Automation::Provider::IToggleProvider>()
            .Toggle();
          _ready = [input, checked] { return input.IsChecked().GetBoolean() == checked; };
          return;
        }
      }

      throw winrt::hresult_invalid_argument{L"Missing clear action naming the field and target count"};
    }

    void stageAlbumClear() { toggleFieldClear(ao::rt::TrackField::Album, true); }

    void saveProperties()
    {
      invokeDialogButton(L"PrimaryButton");
      _ready = [this] { return _dialogClosed; };
    }

    void checkAlbums(std::string const& first,
                     std::string const& second,
                     std::string const& outside,
                     std::uint64_t commits)
    {
      auto snapshot = _runtimePtr->library().snapshot();
      check(snapshot.trackField(_trackId, ao::rt::TrackField::Album) == ao::rt::TrackFieldRawValue{first},
            "durable first album matches the captured edit");
      check(snapshot.trackField(_emptyTrackId, ao::rt::TrackField::Album) == ao::rt::TrackFieldRawValue{second},
            "durable second album matches its own edit");
      check(snapshot.trackField(_outsideTrackId, ao::rt::TrackField::Album) == ao::rt::TrackFieldRawValue{outside},
            "durable third album matches its own edit");
      check(snapshot.revision() == _batchBaseline + commits && _publications.size() == _batchPublications + commits,
            "one durable publication per batch save, none on no-op, staging or discard");
    }

    void planBatchPropertiesScenarios()
    {
      _steps.emplace_back(
        [this]
        {
          _batchBaseline = _runtimePtr->library().snapshot().revision();
          _batchPublications = _publications.size();
          openProperties({_trackId, _emptyTrackId});
        });
      _steps.emplace_back(
        [this]
        {
          auto album = albumInput();
          check(album.IsEnabled() && album.Text().empty() &&
                  album.PlaceholderText() ==
                    winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, MessageId::TrackMultipleValues)),
                "mixed album is a writable placeholder, not text");
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "native Save is disabled for a no-op");
          invokeDialogButton(L"CloseButton");
          _ready = [this] { return _dialogClosed; };
        });
      _steps.emplace_back(
        [this]
        {
          releaseClosedDialog();
          checkAlbums("First album", "Second album", "Outside album", 0);
          openProperties({_trackId, _emptyTrackId});
        });
      _steps.emplace_back([this] { setTitle(L"Only title"); });
      _steps.emplace_back([this] { saveProperties(); });
      _steps.emplace_back(
        [this]
        {
          releaseClosedDialog();
          checkAlbums("First album", "Second album", "Outside album", 1);

          for (auto id : {_trackId, _emptyTrackId})
          {
            check(_runtimePtr->library().snapshot().trackField(id, ao::rt::TrackField::Title) ==
                    ao::rt::TrackFieldRawValue{std::string{"Only title"}},
                  "other field saved on both targets");
          }

          openProperties({_trackId, _emptyTrackId});
        });
      _steps.emplace_back([this] { setText(albumInput(), L"Temporary"); });
      _steps.emplace_back(
        [this]
        {
          check(_workflowDialog.IsPrimaryButtonEnabled(), "mixed replacement admits Save");
          setText(albumInput(), L"");
        });
      _steps.emplace_back(
        [this]
        {
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "erasing the proposed album revokes its replacement");
          setTitle(L"After erase");
        });
      _steps.emplace_back([this] { saveProperties(); });
      _steps.emplace_back(
        [this]
        {
          releaseClosedDialog();
          checkAlbums("First album", "Second album", "Outside album", 2);
          openProperties({_trackId, _emptyTrackId});
        });
      _steps.emplace_back([this] { stageAlbumClear(); });
      _steps.emplace_back(
        [this]
        {
          checkAlbums("First album", "Second album", "Outside album", 2);
          invokeDialogButton(L"CloseButton");
        });
      _steps.emplace_back(
        [this]
        {
          invoke(MessageId::WinUiTrackPropertiesDiscard);
          _ready = [this] { return _dialogClosed; };
        });
      _steps.emplace_back(
        [this]
        {
          releaseClosedDialog();
          checkAlbums("First album", "Second album", "Outside album", 2);
          openProperties({_trackId, _emptyTrackId});
        });
      _steps.emplace_back([this] { setText(albumInput(), L"Superseded album"); });
      _steps.emplace_back([this] { stageAlbumClear(); });
      _steps.emplace_back([this] { toggleFieldClear(ao::rt::TrackField::Album, false); });
      _steps.emplace_back(
        [this]
        {
          auto const album = albumInput();
          check(album.Text().empty() && !album.IsSuggestionListOpen() && !_workflowDialog.IsPrimaryButtonEnabled(),
                "withdrawing clear restores the mixed album placeholder, not superseded input");
          setText(textField(ao::rt::TrackField::Year), L"bad year");
        });
      _steps.emplace_back([this] { toggleFieldClear(ao::rt::TrackField::Year, true); });
      _steps.emplace_back(
        [this]
        {
          check(_workflowDialog.IsPrimaryButtonEnabled(), "explicit clear supersedes invalid year input");
          toggleFieldClear(ao::rt::TrackField::Year, false);
        });
      _steps.emplace_back(
        [this]
        {
          check(textField(ao::rt::TrackField::Year).Text() == L"1997" && !_workflowDialog.IsPrimaryButtonEnabled(),
                "withdrawing clear restores the common year, not its old validation error");
          auto year = textField(ao::rt::TrackField::Year);
          check(year.Focus(FocusState::Programmatic), "batch numeric editor takes focus");
          setText(year, L"00");
        });
      _steps.emplace_back(
        [this]
        {
          check(textField(ao::rt::TrackField::Year).Text() == L"00" && !_workflowDialog.IsPrimaryButtonEnabled(),
                "ordinary typing can continue after a numeric zero without staging a clear");
          check(suggestedField(ao::rt::TrackField::Genre).Focus(FocusState::Programmatic),
                "focus leaves the numeric editor");
        });
      _steps.emplace_back(
        [this]
        {
          check(textField(ao::rt::TrackField::Year).Text() == L"1997",
                "a withdrawn batch zero displays the captured year on blur");
          setText(suggestedField(ao::rt::TrackField::Genre), L"Jazz");
        });
      _steps.emplace_back([this] { saveProperties(); });
      _steps.emplace_back(
        [this]
        {
          releaseClosedDialog();
          checkAlbums("First album", "Second album", "Outside album", 3);

          for (auto id : {_trackId, _emptyTrackId})
          {
            auto snapshot = _runtimePtr->library().snapshot();
            check(
              snapshot.trackField(id, ao::rt::TrackField::Year) == ao::rt::TrackFieldRawValue{std::uint16_t{1997}} &&
                snapshot.trackField(id, ao::rt::TrackField::Genre) == ao::rt::TrackFieldRawValue{std::string{"Jazz"}},
              "only the unrelated genre edit was saved on each original target");
          }

          auto viewRes = _runtimePtr->workspace().navigate({.target = ao::rt::GlobalViewKind::AllTracks});

          if (!viewRes || !_runtimePtr->views().setSelection(*viewRes, {_trackId, _emptyTrackId}))
          {
            throw winrt::hresult_invalid_argument{L"Could not select the original two targets"};
          }

          openProperties(_runtimePtr->views().trackListState(*viewRes).selection);
        });
      _steps.emplace_back([this] { setText(albumInput(), L"Together"); });
      _steps.emplace_back([this] { setText(auxiliaryInput(MessageId::WinUiTrackPropertiesTags), L"The\u0301"); });
      _steps.emplace_back(
        [this]
        {
          check(isUnstagedInputHintVisible() && !_workflowDialog.IsPrimaryButtonEnabled(),
                "pending tag text explains why a staged album cannot be saved yet");
          setText(pendingCustomValue(), L"Unadded value");
        });
      _steps.emplace_back([this] { stageAuxiliaryInput(MessageId::WinUiTrackPropertiesTags); });
      _steps.emplace_back(
        [this]
        {
          check(isUnstagedInputHintVisible() && !_workflowDialog.IsPrimaryButtonEnabled(),
                "adding the tag leaves the hint while the custom value alone is pending");
          setText(pendingCustomValue(), L"");
        });
      _steps.emplace_back(
        [this]
        {
          check(!isUnstagedInputHintVisible() && _workflowDialog.IsPrimaryButtonEnabled(),
                "erasing the last pending input hides the hint and restores Save for other edits");
          setText(auxiliaryInput(MessageId::WinUiTrackPropertiesTags), L"Thé");
        });
      _steps.emplace_back([this] { stageAuxiliaryInput(MessageId::WinUiTrackPropertiesTags); });
      _steps.emplace_back(
        [this]
        {
          auto const labels = controls<TextBlock>();
          check(std::ranges::count_if(labels, [](TextBlock const& text) { return text.Text() == L"Thé"; }) == 1 &&
                  std::ranges::none_of(labels, [](TextBlock const& text) { return text.Text() == L"The\u0301"; }),
                "equivalent tag spellings stage exactly one canonical native row before Save");
          setText(auxiliaryInput(MessageId::WinUiTrackPropertiesCustomKey), L"Edition");
        });
      _steps.emplace_back(
        [this]
        {
          check(isUnstagedInputHintVisible(), "pending custom key alone explains disabled Save");
          stageAuxiliaryInput(MessageId::WinUiTrackPropertiesCustomKey);
        });
      _steps.emplace_back(
        [this]
        {
          check(!isUnstagedInputHintVisible() && _workflowDialog.IsPrimaryButtonEnabled(),
                "adding the last pending input dismisses the hint");
          setText(auxiliaryInput(MessageId::WinUiTrackPropertiesCustomKey), L" Cafe\u0301 ");
          setText(pendingCustomValue(), L"Corrected value");
        });
      _steps.emplace_back([this] { stageAuxiliaryInput(MessageId::WinUiTrackPropertiesCustomKey, "Café"); });
      _steps.emplace_back(
        [this]
        {
          auto input = auxiliaryInput(MessageId::WinUiTrackPropertiesCustomKey);
          check(input.Text() == L" Cafe\u0301 " && pendingCustomValue().Text() == L"Corrected value" &&
                  !_workflowDialog.IsPrimaryButtonEnabled(),
                "equivalent partial key is rejected without losing either input");
          check(!isUnstagedInputHintVisible(), "the Add rejection takes precedence over the pending-input hint");
          setText(input, L"Re\u0301sume\u0301");
        });
      _steps.emplace_back([this] { stageAuxiliaryInput(MessageId::WinUiTrackPropertiesCustomKey); });
      _steps.emplace_back(
        [this]
        {
          setText(auxiliaryInput(MessageId::WinUiTrackPropertiesCustomKey), L"Résumé");
          setText(pendingCustomValue(), L"Another value");
        });
      _steps.emplace_back([this] { stageAuxiliaryInput(MessageId::WinUiTrackPropertiesCustomKey, "Résumé"); });
      _steps.emplace_back(
        [this]
        {
          auto input = auxiliaryInput(MessageId::WinUiTrackPropertiesCustomKey);
          check(input.Text() == L"Résumé" && pendingCustomValue().Text() == L"Another value" &&
                  !_workflowDialog.IsPrimaryButtonEnabled(),
                "equivalent staged key is also rejected and correctable");
          setText(textField(ao::rt::TrackField::Year), L"2010x");
        });
      _steps.emplace_back(
        [this]
        {
          check(isTextVisible(customKeyRejection("Résumé")), "unrelated scalar validation does not hide Add rejection");
          setText(textField(ao::rt::TrackField::Year), L"1997");
        });
      _steps.emplace_back(
        [this]
        {
          check(isTextVisible(customKeyRejection("Résumé")), "correcting the scalar does not dismiss Add rejection");
          setText(auxiliaryInput(MessageId::WinUiTrackPropertiesCustomKey), L"Another key");
        });
      _steps.emplace_back([this] { stageAuxiliaryInput(MessageId::WinUiTrackPropertiesCustomKey); });
      _steps.emplace_back(
        [this]
        {
          auto const view = _runtimePtr->workspace().snapshot().activeViewId;
          check(static_cast<bool>(_runtimePtr->views().setSelection(view, {_outsideTrackId})),
                "live selection changed before Save");
          saveProperties();
        });
      _steps.emplace_back(
        [this]
        {
          releaseClosedDialog();
          checkAlbums("Together", "Together", "Outside album", 4);

          {
            auto snapshot = _runtimePtr->library().snapshot();

            for (auto id : {_trackId, _emptyTrackId})
            {
              check(snapshot.selectionTags(std::array{id}) == std::vector<std::string>{"Thé"},
                    "one canonical tag on each original target");
              check(snapshot.trackCustomMetadataValue(id, "Edition") == "",
                    "empty custom value is present, not a deletion");
              check(snapshot.trackCustomMetadataValue(id, "Résumé") == "Corrected value" &&
                      snapshot.trackCustomMetadataValue(id, "Another key") == "Another value",
                    "corrected keys commit their retained inputs without overwriting one another");
            }

            check(snapshot.trackCustomMetadataValue(_trackId, "Café") == "Original partial value" &&
                    !snapshot.trackCustomMetadataValue(_emptyTrackId, "Café"),
                  "rejected canonical alias never changes the partial key");

            check(!snapshot.trackCustomMetadataValue(_outsideTrackId, "Edition") &&
                    snapshot.selectionTags(std::array{_outsideTrackId}).empty(),
                  "no auxiliary edits reached the newly selected track");
          }

          openProperties({_trackId, _emptyTrackId});
        });
      _steps.emplace_back([this] { setText(albumInput(), L"Temporary common"); });
      _steps.emplace_back([this] { setText(albumInput(), L""); });
      _steps.emplace_back(
        [this]
        {
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "erasing a common batch input also preserves originals");
          setTitle(L"After common erase");
        });
      _steps.emplace_back([this] { saveProperties(); });
      _steps.emplace_back(
        [this]
        {
          releaseClosedDialog();
          checkAlbums("Together", "Together", "Outside album", 5);
          // A new mixed pair exercises clear/save without reseeding or bypassing the coordinator.
          openProperties({_trackId, _outsideTrackId});
        });
      _steps.emplace_back([this] { stageAlbumClear(); });
      _steps.emplace_back([this] { saveProperties(); });
      _steps.emplace_back(
        [this]
        {
          releaseClosedDialog();
          checkAlbums("", "Together", "", 6);
          openProperties({_trackId, _emptyTrackId});
        });
      _steps.emplace_back([this] { setText(textField(ao::rt::TrackField::Year), L"2010"); });
      _steps.emplace_back(
        [this]
        {
          check(_workflowDialog.IsPrimaryButtonEnabled(), "valid year is the only staged edit");
          setText(textField(ao::rt::TrackField::Year), L"2010x");
        });
      _steps.emplace_back(
        [this]
        {
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "the invalid year blocks Save");
          mutateExternally();
        });
      _steps.emplace_back(
        [this]
        {
          auto year = textField(ao::rt::TrackField::Year);
          check(_externalAccepted && !year.IsEnabled() && year.Text() == L"2010x", "stale input remains frozen");
          invokeDialogButton(L"CloseButton");
        });
      _steps.emplace_back(
        [this]
        {
          auto const stale =
            winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, MessageId::WinUiTrackPropertiesStale));
          check(std::ranges::any_of(
                  controls<TextBlock>(), [&stale](TextBlock const& text) { return text.Text() == stale; }),
                "a discard check on the sole invalid field must not overwrite the stale diagnostic");
          invoke(MessageId::WinUiTrackPropertiesDiscard);
          _ready = [this] { return _dialogClosed; };
        });
      _steps.emplace_back(
        [this]
        {
          releaseClosedDialog();
          auto snapshot = _runtimePtr->library().snapshot();

          for (auto id : {_trackId, _emptyTrackId})
          {
            check(snapshot.trackField(id, ao::rt::TrackField::Year) == ao::rt::TrackFieldRawValue{std::uint16_t{1997}},
                  "discarding the invalid year must not write its previous valid replacement");
          }
        });
    }

    bool hasPropertiesErrorDetail(std::string_view detail) const
    {
      return std::ranges::any_of(controls<TextBlock>(),
                                 [detail](TextBlock const& text)
                                 { return winrt::to_string(text.Text()).contains(detail); });
    }

    void planFailedPropertiesSave()
    {
      // A three-byte UTF-8 character reaches the storage limit while keeping the native text short enough.
      auto const oversizedTitle = winrt::hstring{std::wstring(21'846, L'\u2014')};
      auto const reloadLabel =
        winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, MessageId::WinUiTrackPropertiesReload));
      auto const question =
        winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, MessageId::WinUiTrackPropertiesDiscardQuestion));
      _steps.emplace_back([this] { openProperties({_trackId}); });
      _steps.emplace_back([this, oversizedTitle] { setTitle(oversizedTitle); });
      _steps.emplace_back(
        [this, reloadLabel]
        {
          invokeDialogButton(L"PrimaryButton");
          _ready = [this, reloadLabel]
          {
            auto const reloadAvailable = [&reloadLabel](Button const& candidate)
            {
              auto const label = candidate.Content().try_as<TextBlock>();
              return label && label.Text() == reloadLabel && candidate.IsEnabled();
            };
            return _dialogClosed || std::ranges::any_of(controls<Button>(), reloadAvailable);
          };
        });
      _steps.emplace_back(
        [this, oversizedTitle, question]
        {
          if (_dialogClosed)
          {
            fail("a rejected title write must leave Properties open");
            return;
          }

          check(
            hasPropertiesErrorDetail("Hot title length 65538"), "real write rejection keeps the exact failure visible");
          check(!_workflowDialog.IsPrimaryButtonEnabled() && !titleInput().IsEnabled() &&
                  titleInput().Text() == oversizedTitle,
                "write failure freezes the uncommitted draft and forbids retry");
          invokeDialogButton(L"CloseButton");
          _ready = [this, question] { return _dialogClosed || isTextVisible(question); };
        });
      _steps.emplace_back(
        [this]
        {
          if (_dialogClosed)
          {
            fail("closing a rejected write must ask whether to discard its draft");
            return;
          }

          check(hasPropertiesErrorDetail("Hot title length 65538"),
                "discard validation cannot replace the failed-write diagnostic");
          invoke(MessageId::WinUiTrackPropertiesDiscard);
          _ready = [this] { return _dialogClosed; };
        });
      _steps.emplace_back(
        [this]
        {
          releaseClosedDialog();
          check(_runtimePtr->library().snapshot().trackField(_trackId, ao::rt::TrackField::Title) ==
                  ao::rt::TrackFieldRawValue{std::string{"External title"}},
                "discarding a rejected write leaves the stored title unchanged");
        });
    }

    void planFailedPropertiesReload()
    {
      // Delete a seeded fixture only after all other scenarios have finished using the captured tracks.
      auto const draft = winrt::hstring{L"Discard this draft before reload"};
      auto const question =
        winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, MessageId::WinUiTrackPropertiesDiscardQuestion));
      _steps.emplace_back([this] { openProperties({_trackId, _emptyTrackId}); });
      _steps.emplace_back([this, draft] { setTitle(draft); });
      _steps.emplace_back(
        [this]
        {
          check(_workflowDialog.IsPrimaryButtonEnabled() && !controls<CheckBox>().empty(),
                "bound batch has a staged edit and available Clear controls");
          deleteCapturedTrack(_emptyTrackId);
        });
      _steps.emplace_back(
        [this, question]
        {
          check(_externalAccepted && !_runtimePtr->library().snapshot().containsTrack(_emptyTrackId),
                "the external command removed one captured target");
          invoke(MessageId::WinUiTrackPropertiesReload);
          _ready = [this, question] { return isTextVisible(question); };
        });
      _steps.emplace_back(
        [this, question]
        {
          invoke(MessageId::WinUiTrackPropertiesDiscard);
          _ready = [this, question] { return _dialogClosed || !isTextVisible(question); };
        });
      _steps.emplace_back(
        [this, draft, question]
        {
          if (_dialogClosed)
          {
            fail("failed reload must leave unbound Properties open");
            return;
          }

          auto const bindingRes = _runtimePtr->library().bindTrackTargets(std::array{_trackId, _emptyTrackId});

          if (bindingRes || bindingRes.error().code != ao::Error::Code::NotFound)
          {
            fail("deleted Properties target must fail binding");
            return;
          }

          check(
            hasPropertiesErrorDetail(bindingRes.error().message), "failed reload exposes the missing target detail");
          check(!isTextVisible(question) && controls<CheckBox>().empty(),
                "failed reload discards the old confirmation and batch Clear controls");
          check(
            std::ranges::none_of(controls<TextBox>(), [&draft](TextBox const& input) { return input.Text() == draft; }),
            "failed reload does not retain the confirmed discarded draft");
          check(!_workflowDialog.IsPrimaryButtonEnabled() && button(MessageId::WinUiTrackPropertiesReload).IsEnabled(),
                "failed reload disables Save but keeps Reload available");
          invokeDialogButton(L"CloseButton");
          _ready = [this, question] { return _dialogClosed || isTextVisible(question); };
        });
      _steps.emplace_back(
        [this]
        {
          check(_dialogClosed, "failed reload closes without a second discard confirmation");
          releaseClosedDialog();
        });
    }

    void planScenarios()
    {
      _steps.emplace_back([this] { show(emptySections(), ao::uimodel::trackCreditScope(CreditKind::Soloist)); });
      _steps.emplace_back(
        [this]
        {
          check(inputs(MessageId::TrackCreditName).empty(), "zero-entry category renders no invented row");
          check(button(MessageId::TrackCreditsAdd).IsTabStop(), "Add is keyboard reachable");
          invoke(MessageId::TrackCreditsAdd);
        });
      _steps.emplace_back(
        [this]
        {
          auto names = inputs(MessageId::TrackCreditName);
          check(names.size() == 1, "one-entry category uses the list editor");
          check(!button(MessageId::TrackCreditsCommit).IsEnabled(), "blank row disables Save");
          check(!_model.validationErrors().empty(), "invalid row retains shared validation errors");
          bool visibleError = false;

          for (auto const& text : controls<TextBlock>())
          {
            if (Automation::AutomationProperties::GetLiveSetting(text) ==
                  Automation::Peers::AutomationLiveSetting::Polite &&
                text.Visibility() == Visibility::Visible && !text.Text().empty())
            {
              visibleError = true;
            }
          }

          check(visibleError, "invalid row exposes a visible native live validation message");

          if (names.size() != 1)
          {
            return;
          }

          check(isFocusedWithin(names[0]), "new invalid row receives native focus");
          auto focusOptions = Input::FindNextElementOptions{};
          focusOptions.SearchRoot(_scroll);
          check(Input::FocusManager::TryMoveFocus(Input::FocusNavigationDirection::Next, focusOptions),
                "native forward focus navigation succeeds");
          auto roles = inputs(MessageId::TrackCreditRole);

          if (roles.size() != 1)
          {
            fail("one-entry category must expose one native role input");
            return;
          }

          setText(names[0], L"Anne");
          setText(roles[0], L"violin");
        });
      _steps.emplace_back(
        [this]
        {
          auto const roles = inputs(MessageId::TrackCreditRole);

          if (roles.size() != 1)
          {
            fail("one-entry category must retain its native role input");
            return;
          }

          check(isFocusedWithin(roles[0]), "Tab-order successor is the role input");
          check(
            _model.entries() == std::vector<Credit>{{.name = "Anne", .kind = CreditKind::Soloist, .role = "violin"}},
            "native name and role changes preserve locked kind");
          check(button(MessageId::TrackCreditsCommit).IsEnabled(), "valid row enables child Save");

          for (auto const& kind : controls<ComboBox>())
          {
            check(kind.Visibility() == Visibility::Collapsed, "category editor hides kind controls");
          }

          invoke(MessageId::TrackCreditsAdd);
        });
      _steps.emplace_back(
        [this]
        {
          auto names = inputs(MessageId::TrackCreditName);
          check(names.size() == 2, "many-entry category keeps separate duplicate rows");

          auto roles = inputs(MessageId::TrackCreditRole);

          if (names.size() != 2 || roles.size() != 2)
          {
            fail("two-entry category must expose two complete native rows");
            return;
          }

          setText(names[1], L"Anne");
          setText(roles[1], L"piano");
        });
      _steps.emplace_back([this] { invoke(MessageId::TrackCreditMoveUp, 1); });
      _steps.emplace_back(
        [this]
        {
          check(
            _model.entries() == std::vector<Credit>{{.name = "Anne", .kind = CreditKind::Soloist, .role = "piano"},
                                                    {.name = "Anne", .kind = CreditKind::Soloist, .role = "violin"}},
            "reorder preserves duplicate names and distinct roles");
          check(!button(MessageId::TrackCreditMoveUp, 0).IsEnabled(), "first entry cannot cross its kind boundary");
          checkRowCommandNames(2);
          check(automationName(button(MessageId::TrackCreditMoveUp, 1)) == L"Move credit 2 up" &&
                  automationName(button(MessageId::TrackCreditDelete, 0)) == L"Delete credit 1",
                "English row command names identify their one-based row");
          auto const names = inputs(MessageId::TrackCreditName);

          if (names.size() != 2)
          {
            fail("reorder must retain two native name inputs");
            return;
          }

          check(isFocusedWithin(names[0]), "reordered row regains focus");
          check(_controlPtr->element().ActualWidth() > 0.0 && _controlPtr->element().ActualWidth() <= 320.5,
                "loaded editor fits constrained 320-effective-pixel content width");

          for (auto const& command : controls<Button>())
          {
            check(command.ActualWidth() <= 320.5, "commands fit constrained content width");
          }

          invoke(MessageId::TrackCreditsCommit);
        });
      _steps.emplace_back(
        [this]
        {
          check(_optAccepted && _optAccepted->optCredits && _optAccepted->optCredits->entries.size() == 2,
                "native Save emits an owning duplicate-preserving replacement");
          check(_optAccepted && _optAccepted->optCredits &&
                  _optAccepted->optCredits->kinds == ao::uimodel::trackCreditScope(CreditKind::Soloist),
                "category Save stays scoped");
          auto sections = emptySections();
          sections[0].optValue =
            std::vector<Credit>{{.name = "Conductor", .kind = CreditKind::Conductor, .role = "lead"}};
          sections[3].optValue =
            std::vector<Credit>{{.name = "Existing", .kind = CreditKind::Performer, .role = "piano"}};
          show(sections, ao::uimodel::allTrackCreditKinds());
        });
      _steps.emplace_back(
        [this]
        {
          auto kinds = controls<ComboBox>();
          check(kinds.size() == 3, "all-kind editor has row kind selectors and an Add kind selector");

          if (kinds.size() != 3)
          {
            fail("all-kind editor must expose all three kind selectors");
            return;
          }

          // Row selectors precede the Add selector in visual order.
          check(kinds[2].SelectedIndex() == static_cast<std::int32_t>(CreditKind::Performer),
                "full-scope Add selector starts at the shared Performer default");
          auto const draft = _model.entries();
          _controlPtr->setEnabled(false);

          for (auto const& input : controls<Control>())
          {
            check(!input.IsEnabled(), "disabled owner freezes every native descendant control");
          }

          auto names = inputs(MessageId::TrackCreditName);
          auto roles = inputs(MessageId::TrackCreditRole);

          if (names.size() != 2 || roles.size() != 2)
          {
            fail("disabled owner must retain both complete native rows");
            return;
          }

          _disabledDraft = draft;
          setText(names[0], L"Rejected name");
          setText(roles[0], L"Rejected role");
          kinds[0].SelectedIndex(3);
        });
      _steps.emplace_back(
        [this]
        {
          check(_model.entries() == _disabledDraft && _model.isEditing() && !_optAccepted,
                "disabled native property callbacks cannot mutate or accept the retained draft");
          _controlPtr->refresh();

          for (auto const& input : controls<Control>())
          {
            check(!input.IsEnabled(), "refresh cannot re-enable disabled descendants");
          }

          _controlPtr->setEnabled(true);
          check(button(MessageId::TrackCreditsAdd).IsEnabled(), "retry restores editable Add");
          check(!button(MessageId::TrackCreditMoveUp, 0).IsEnabled() &&
                  !button(MessageId::TrackCreditMoveDown, 0).IsEnabled(),
                "retry retains the first row's cross-kind movement constraints");
          check(inputs(MessageId::TrackCreditName)[0].Text() == L"Conductor" &&
                  inputs(MessageId::TrackCreditRole)[0].Text() == L"lead",
                "retry presents the retained name and role rather than rejected callbacks");
          // refresh() rebuilt the rows; a selector opens only after its native load.
          _ready = [this] { return controls<ComboBox>().front().IsLoaded(); };
        });
      _steps.emplace_back(
        [this]
        {
          // Drive the row kind selector through its dropdown, not a programmatic SelectedIndex.
          _kindSelector = controls<ComboBox>().front();
          // Dropdown realization can finish without another layout pass of the probe root.
          _dropDownPoll.Start();
          expandCollapse(_kindSelector).Expand();
          _ready = [this]
          {
            auto const item = _kindSelector.ContainerFromIndex(3).try_as<ComboBoxItem>();
            return _kindSelector.IsDropDownOpen() && item && item.IsLoaded();
          };
        });
      _steps.emplace_back(
        [this]
        {
          // ComboBox item data peers, not item containers, carry the SelectionItem pattern.
          auto const label = winrt::to_hstring(ao::uimodel::trackCreditKindLabel(*_optCatalog, CreditKind::Performer));
          auto selection = Automation::Provider::ISelectionItemProvider{nullptr};

          for (auto const& child :
               Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(_kindSelector).GetChildren())
          {
            if (auto const pattern = child.GetPattern(Automation::Peers::PatternInterface::SelectionItem);
                pattern && child.GetName() == label)
            {
              selection = pattern.as<Automation::Provider::ISelectionItemProvider>();
            }
          }

          if (!selection)
          {
            fail("open kind dropdown must expose a selectable localized Performer item");
            return;
          }

          selection.Select();

          // A selector that defers commitment applies the highlighted choice when its dropdown closes.
          if (_kindSelector.IsDropDownOpen())
          {
            expandCollapse(_kindSelector).Collapse();
          }

          _ready = [this] { return !_model.entries().empty() && _model.entries().front().name == "Existing"; };
        });
      _steps.emplace_back(
        [this]
        {
          check(_model.entries() ==
                  std::vector<Credit>{{.name = "Existing", .kind = CreditKind::Performer, .role = "piano"},
                                      {.name = "Conductor", .kind = CreditKind::Performer, .role = "lead"}},
                "native kind dropdown choice appends to destination and retains role");
          check(Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(_scroll.XamlRoot()).Size() == 0,
                "kind dropdown closes after its choice is applied");
          check(isFocusedWithin(inputs(MessageId::TrackCreditName)[1]), "reclassified row regains focus");
          _dropDownPoll.Stop();
          _kindSelector = nullptr;
          invoke(MessageId::TrackCreditDelete, 0);
        });
      _steps.emplace_back(
        [this]
        {
          check(_model.entries() ==
                  std::vector<Credit>{{.name = "Conductor", .kind = CreditKind::Performer, .role = "lead"}},
                "native Delete preserves remaining attributes");
          check(controls<ComboBox>().back().SelectedIndex() == static_cast<std::int32_t>(CreditKind::Performer),
                "rebuilt full-scope Add selector keeps the shared default");
          invoke(MessageId::TrackCreditsAdd);
        });
      _steps.emplace_back(
        [this]
        {
          check(_model.entries().size() == 2 && _model.entries()[1].kind == CreditKind::Performer &&
                  _model.entries()[1].name.empty(),
                "full-scope Add without a kind choice appends a Performer row");
          invoke(MessageId::TrackCreditsCancel);
        });
      _steps.emplace_back(
        [this]
        {
          check(_cancelled == 1 && !_optAccepted && !_model.isEditing(),
                "Cancel emits no replacement and ends child draft");
          auto sections = emptySections();
          sections[3] = {.mixed = true};
          show(sections, ao::uimodel::allTrackCreditKinds());
        });
      _steps.emplace_back(
        [this]
        {
          check(inputs(MessageId::TrackCreditName).empty(), "mixed scope never seeds target or union rows");
          check(!button(MessageId::TrackCreditsAdd).IsEnabled(), "mixed scope requires explicit intent before Add");
          check(!button(MessageId::TrackCreditsCommit).IsEnabled(), "untouched mixed scope cannot clear");
          invoke(MessageId::TrackCreditsReplaceScope);
        });
      _steps.emplace_back(
        [this]
        {
          check(button(MessageId::TrackCreditsAdd).IsEnabled(), "explicit replacement unlocks Add");
          check(!button(MessageId::TrackCreditsCommit).IsEnabled(), "empty mixed replacement is not an implicit clear");
          invoke(MessageId::TrackCreditsClear);
        });
      _steps.emplace_back(
        [this]
        {
          check(button(MessageId::TrackCreditsCommit).IsEnabled(), "explicit scoped Clear is committable");
          _controlPtr->setEnabled(false);

          for (auto const& input : controls<Control>())
          {
            check(!input.IsEnabled(), "stale/busy owner disables every native child without dropping draft");
          }

          check(_model.isEditing() && _model.canCommit(), "disabled presentation retains explicit clear draft");
          _controlPtr->setEnabled(true);
          invoke(MessageId::TrackCreditsCommit);
        });
      _steps.emplace_back(
        [this]
        {
          check(_optAccepted && _optAccepted->optCredits && _optAccepted->optCredits->kinds.all() &&
                  _optAccepted->optCredits->entries.empty(),
                "explicit mixed Clear emits exactly the full selected empty replacement");
        });
    }

    // main owns the result until Application::Start returns after native shutdown.
    std::int32_t& _exitCode;
    TempRoot _temp;
    std::optional<ao::i18n::MessageCatalog> _optCatalog;
    std::unique_ptr<ao::rt::AppRuntime> _runtimePtr;
    // Owned by AppRuntime, valid until completeClosing followed by runtime destruction.
    ao::winui::DispatcherQueueExecutor* _runtimeExecutor = nullptr;
    std::unique_ptr<ao::winui::TrackDetailControl> _detailPtr;
    std::unique_ptr<ao::winui::TrackPropertiesCoordinator> _coordinatorPtr;
    ao::TrackId _trackId{};
    ao::TrackId _emptyTrackId{};
    ao::TrackId _outsideTrackId{};
    std::uint64_t _batchBaseline = 0;
    std::size_t _batchPublications = 0;
    Button _detailMetadataHeader{nullptr};
    Button _detailShowEmpty{nullptr};
    std::vector<ao::TrackId> _detailExpectedIds;
    std::vector<ao::TrackId> _detailCallbackIds;
    std::optional<std::bitset<ao::library::kCreditKindCount>> _optDetailCallbackScope;
    bool _probeDetailActions = false;
    bool _userInputObserved = false;
    bool _suggestionChosenObserved = false;
    std::vector<AutoSuggestBox::SuggestionChosen_revoker> _suggestionRevokers;
    AutoSuggestBox _retiredName{nullptr};
    AutoSuggestBox _retiredRole{nullptr};
    ComboBox _retiredKind{nullptr};
    ComboBox _kindSelector{nullptr};
    Button _retiredDelete{nullptr};
    TextBox _typingBox{nullptr};
    std::uint64_t _baselineRevision = 0;
    std::vector<ao::rt::LibraryChangeSet> _publications;
    ao::async::Subscription _publicationSub;
    ao::async::LifetimeScope _workflowTasks;
    std::function<bool()> _ready;
    ContentDialog _workflowDialog{nullptr};
    FrameworkElement::LayoutUpdated_revoker _dialogLayoutRevoker{};
    ContentDialog::Closed_revoker _dialogClosedRevoker{};
    std::vector<TextBox::TextChanged_revoker> _fieldTextRevokers;
    bool _expectDialog = false;
    bool _dialogClosed = false;
    bool _externalCompleted = false;
    bool _externalAccepted = false;
    ao::uimodel::TrackCreditsEditorModel _model;
    std::unique_ptr<ao::winui::TrackCreditsEditorControl> _controlPtr;
    std::optional<ao::rt::MetadataPatch> _optAccepted;
    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> _providerPtr;
    Window _window{nullptr};
    ScrollViewer _scroll{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _timeout{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _dropDownPoll{nullptr};
    FrameworkElement::LayoutUpdated_revoker _layoutRevoker{};
    std::vector<AutoSuggestBox::TextChanged_revoker> _textRevokers;
    std::vector<bool> _textObservations;
    std::vector<Credit> _disabledDraft;
    std::vector<std::function<void()>> _steps;
    bool _queued = false;
    std::size_t _step = 0;
    std::size_t _failures = 0;
    std::size_t _cancelled = 0;
    bool _finished = false;
  };
} // namespace

int main()
{
  try
  {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    std::int32_t exitCode = 1;
    Application::Start([&exitCode](auto const&) { std::ignore = winrt::make<AobusCreditsProbeApplication>(exitCode); });
    return exitCode;
  }
  catch (winrt::hresult_error const& error)
  {
    std::cerr << "WinUI Credits probe startup failed: " << winrt::to_string(error.message()) << '\n';
    return 2;
  }
}

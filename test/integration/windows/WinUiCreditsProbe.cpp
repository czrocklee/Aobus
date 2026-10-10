// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Loaded native Credits controls, driven through XAML properties, automation
// Invoke and native focus navigation, followed by the shipping detail control
// and Properties coordinator over a disposable runtime. No physical input or
// MainWindow modal-admission coverage is claimed.
#include "XamlMetaDataProvider.h"
#include "app/DispatcherQueueExecutor.h"
#include "platform/StringResources.h"
#include "track/TrackCreditsEditorControl.h"
#include "track/TrackDetailControl.h"
#include "track/TrackPropertiesCoordinator.h"
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
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
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>
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
  std::int32_t exitCode = 1;

  class TempRoot final
  {
  public:
    TempRoot()
      : _path{std::filesystem::temp_directory_path() /
              std::format("aobus-winui-credits-probe-{}", std::chrono::steady_clock::now().time_since_epoch().count())}
    {
      std::filesystem::create_directories(_path / "database");
    }
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

    if (auto control = root.try_as<T>())
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
    AobusCreditsProbeApplication()
    {
      UnhandledException(
        [this](auto const&, UnhandledExceptionEventArgs const& args)
        {
          args.Handled(true);
          fail(winrt::to_string(args.Message()));
        });
      Application::LoadComponent(*this, winrt::Windows::Foundation::Uri{L"ms-appx:///App.xaml"});
    }

    void OnLaunched(LaunchActivatedEventArgs const&)
    {
      auto catalogRes = ao::i18n::MessageCatalog::create("en");
      auto seedRes = seedLibrary();
      auto languageRes = ao::winui::configureResourceLanguage("en");

      if (!catalogRes || !seedRes || !languageRes)
      {
        fail(!catalogRes ? catalogRes.error().message
             : !seedRes  ? seedRes.error().message
                         : languageRes.error().message);
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
      planScenarios();
      planFeedbackScenarios();
      planWorkflowScenarios();
      _window.Activate();
      next();
    }

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

      for (auto const& observed : _textObservations)
      {
        if (!observed)
        {
          return false;
        }
      }

      return true;
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
      exitCode = _failures == 0 && _step == _steps.size() ? 0 : 1;
      std::cerr << std::format("WinUI Credits probe: {} steps, {} failures, exit {}\n", _step, _failures, exitCode);

      if (_timeout)
      {
        _timeout.Stop();
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

      if (_runtimeExecutor)
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
      collect(_workflowDialog ? _workflowDialog.as<DependencyObject>()
              : _controlPtr   ? _controlPtr->element().as<DependencyObject>()
                              : _scroll.as<DependencyObject>(),
              found);
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
          if (std::ranges::find(found, control) == found.end())
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
        auto text = candidate.Content().try_as<TextBlock>();

        if (text && text.Text() == label && index++ == occurrence)
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

    void show(ao::uimodel::TrackCreditSections const& sections, std::bitset<ao::library::kCreditKindCount> const scope)
    {
      _controlPtr.reset();
      _optAccepted.reset();
      auto beginRes = _model.begin(sections, scope);

      if (!beginRes)
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
          builder.metadata().title("Original title").credits(credits);
          builder.property().uri("offline-probe.flac");
          auto createdRes = write.tracks().create(builder, ao::library::FileManifestBuilder::makeEmpty());

          if (!createdRes)
          {
            return std::unexpected{createdRes.error()};
          }

          _trackId = *createdRes;
          auto emptyBuilder = ao::library::TrackBuilder::makeEmpty();
          emptyBuilder.property().uri("empty-probe.flac");
          auto emptyRes = write.tracks().create(emptyBuilder, ao::library::FileManifestBuilder::makeEmpty());

          if (!emptyRes)
          {
            return std::unexpected{emptyRes.error()};
          }

          _emptyTrackId = *emptyRes;
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
                winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, MessageId::AppKitDiscardChanges));
              for (auto const& command : controls<Button>())
              {
                if (auto text = command.Content().try_as<TextBlock>(); text && text.Text() == label)
                {
                  return true;
                }
              }

              return false;
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

    TextBox titleInput()
    {
      auto const label = winrt::to_hstring(ao::uimodel::trackFieldLabel(*_optCatalog, ao::rt::TrackField::Title));

      for (auto const& input : controls<TextBox>())
      {
        if (input.Header() && winrt::unbox_value<winrt::hstring>(input.Header()) == label)
        {
          return input;
        }
      }

      throw winrt::hresult_invalid_argument{L"Properties title input is missing"};
    }

    void setTitle(winrt::hstring const& text)
    {
      auto input = titleInput();
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
            _coordinatorPtr =
              std::make_unique<ao::winui::TrackPropertiesCoordinator>(ao::winui::TrackPropertiesCoordinatorConfig{
                .xamlRoot = _scroll.XamlRoot(),
                .asyncRuntime = _runtimePtr->async(),
                .library = _runtimePtr->library(),
                .workspace = _runtimePtr->workspace(),
                .completion = _runtimePtr->completion(),
                .textCatalog = *_optCatalog,
                .trackIds = std::move(ids),
                .optCreditsScope = scope,
              });
            auto presentRes = _coordinatorPtr->present();

            if (!presentRes)
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
          },
        },
        _runtimePtr->workspace().detailProjection(ao::rt::ExplicitSelectionTarget{std::move(ids)}));
      _ready = [rows] { return rows.IsLoaded() && rows.ActualWidth() > 0.0; };
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

    static ao::async::Task<void> mutateExternallyAsync(
      AobusCreditsProbeApplication* owner,
      ao::async::Task<ao::Result<ao::rt::TrackAuthoringResult<ao::rt::UpdateTrackMetadataReply>>> submission,
      std::stop_token stopToken)
    {
      auto res = co_await std::move(submission);
      co_await owner->_runtimePtr->async().resumeOnCallbackExecutorAsync(stopToken);

      if (!owner->_finished)
      {
        owner->_externalAccepted = res && res->status == ao::rt::AuthoringStatus::Applied;
        owner->_externalCompleted = true;
        owner->next();
      }
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
      _runtimePtr->async().spawnWithLifetime(
        _workflowTasks,
        [this, submission = std::move(submission)](std::stop_token stopToken) mutable
        { return mutateExternallyAsync(this, std::move(submission), stopToken); },
        "Credits probe external revision");
      _ready = [this] { return _externalCompleted; };
    }

    void checkDetailActions(bool const visible)
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

        for (auto const kind :
             {CreditKind::Conductor, CreditKind::Ensemble, CreditKind::Soloist, CreditKind::Performer})
        {
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

      check(
        count == (visible ? 5 : 0), "Show Empty binds the whole Credits section, retaining all four category actions");
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
      auto input = names.back();
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
      _steps.push_back(
        [this]
        {
          auto sections = emptySections();
          sections[0].optValue = std::vector<Credit>{{.name = "Atlas", .kind = CreditKind::Conductor, .role = "baton"}};
          sections[2].optValue = std::vector<Credit>{{.name = "Beta", .kind = CreditKind::Soloist, .role = "violin"},
                                                     {.name = "Gamma", .kind = CreditKind::Soloist, .role = "cello"}};
          show(sections, ao::uimodel::allTrackCreditKinds());
          _model.focusRow(2);
        });
      _steps.push_back([this] { observeUnchangedHydration(); });
      _steps.push_back(
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
      _steps.push_back(
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
      _steps.push_back(
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
      _steps.push_back([this] { observeUnchangedHydration(); });
      _steps.push_back(
        [this]
        {
          requireNativeRows(3);
          check(_model.focusedRow() == 2 && isFocusedWithin(inputs(MessageId::TrackCreditName)[2]),
                "explicit multirow refresh cannot claim focus through unchanged deferred text");
          editNativeName(L"An");
        });
      _steps.push_back(
        [this]
        {
          check(_userInputObserved && _model.entries()[2].name == "An" && _model.entries()[2].role == "baton",
                "native TextBox input updates only the focused name");
          auto input = inputs(MessageId::TrackCreditName)[2];
          check(input.IsSuggestionListOpen() && input.ItemsSource(), "UserInput retains category completion");
          bool hasAnne = false;

          if (input.ItemsSource())
          {
            for (auto const& item :
                 input.ItemsSource()
                   .as<winrt::Windows::Foundation::Collections::IIterable<winrt::Windows::Foundation::IInspectable>>())
            {
              hasAnne = hasAnne || winrt::unbox_value<winrt::hstring>(item) == L"Anne";
            }
          }

          check(hasAnne, "soloist completion uses the reclassified row kind and seeded native runtime vocabulary");
          check(_typingBox.SelectionStart() == 2 && _typingBox.SelectionLength() == 0 && isFocusedWithin(input),
                "deferred input/completion retains actual focus and caret");
          editNativeName(L"Ann");
        });
      _steps.push_back(
        [this]
        {
          check(_userInputObserved && _model.focusedRow() == 2 && _model.entries()[2].name == "Ann" &&
                  _typingBox.SelectionStart() == 3 && _typingBox.SelectionLength() == 0 &&
                  isFocusedWithin(inputs(MessageId::TrackCreditName)[2]),
                "consecutive native edits preserve logical focus, actual focus, value and caret");
          chooseNativeSuggestion(inputs(MessageId::TrackCreditName)[2], L"Anne");
        });
      _steps.push_back(
        [this]
        {
          check(_suggestionChosenObserved && _model.focusedRow() == 2 &&
                  _model.entries()[2] == Credit{.name = "Anne", .kind = CreditKind::Soloist, .role = "baton"} &&
                  isFocusedWithin(inputs(MessageId::TrackCreditName)[2]),
                "real SuggestionChosen updates the value without losing role, logical or actual focus");
          setText(inputs(MessageId::TrackCreditName)[2], L"Anne edited");
          setText(inputs(MessageId::TrackCreditRole)[2], L"chosen role");
        });
      _steps.push_back(
        [this]
        {
          check(
            _model.entries()[2] == Credit{.name = "Anne edited", .kind = CreditKind::Soloist, .role = "chosen role"},
            "genuine programmatic value edits still mutate names and roles");
          invoke(MessageId::TrackCreditDelete, 2);
        });
      _steps.push_back(
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
      _steps.push_back(
        [this]
        {
          checkDetailActions(false);
          invokeButton(_detailShowEmpty);
        });
      _steps.push_back(
        [this]
        {
          checkDetailActions(true);
          invokeDetailScope(ao::uimodel::allTrackCreditKinds());

          for (auto const kind :
               {CreditKind::Conductor, CreditKind::Ensemble, CreditKind::Soloist, CreditKind::Performer})
          {
            invokeDetailScope(ao::uimodel::trackCreditScope(kind));
          }

          invokeButton(_detailShowEmpty);
        });
      _steps.push_back(
        [this]
        {
          checkDetailActions(false);
          showDetail({_trackId}, true);
        });
      _steps.push_back(
        [this]
        {
          checkDetailActions(true);
          invokeDetailScope(ao::uimodel::trackCreditScope(CreditKind::Ensemble));
          invokeButton(_detailShowEmpty);
        });
      _steps.push_back(
        [this]
        {
          checkDetailActions(true);
          invokeButton(_detailMetadataHeader);
        });
      _steps.push_back(
        [this]
        {
          checkDetailActions(false);
          invokeButton(_detailMetadataHeader);
        });
      _steps.push_back(
        [this]
        {
          checkDetailActions(true);
          showDetail({_trackId, _emptyTrackId}, true);
        });
      _steps.push_back(
        [this]
        {
          checkDetailActions(true);
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
      _steps.push_back(
        [this]
        {
          checkDetailActions(false);
          check(!_detailShowEmpty.IsEnabled(), "no selection disables the shipping Show Empty action");
          check(_detailCallbackIds.empty() && !_optDetailCallbackScope, "no selection emits no Credits callback");
        });
    }

    void planWorkflowScenarios()
    {
      _steps.push_back([this] { showDetail(); });
      _steps.push_back([this] { invokeScope(CreditKind::Soloist); });
      _steps.push_back(
        [this]
        {
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "active Credits child blocks composed Save");
          check(singleCreditName().Text() == L"Anne", "detail entrypoint opens captured scoped baseline");
          setText(singleCreditName(), L"Staged soloist");
          setTitle(L"Composed title");
        });
      _steps.push_back(
        [this]
        {
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "ordinary edits cannot bypass active child guard");
          invoke(MessageId::TrackCreditsCommit);
        });
      _steps.push_back(
        [this]
        {
          checkStored("Original title", expectedCredits("Conductor", "Anne"), 0);
          check(_workflowDialog.IsPrimaryButtonEnabled(), "accepted child enables one composed parent Save");
          invokeScope(CreditKind::Conductor);
        });
      _steps.push_back([this] { setText(singleCreditName(), L"Staged conductor"); });
      _steps.push_back([this] { invoke(MessageId::TrackCreditsCommit); });
      _steps.push_back([this] { invokeScope(CreditKind::Soloist); });
      _steps.push_back(
        [this]
        {
          check(singleCreditName().Text() == L"Staged soloist", "sequential scope reopening uses pending overlay");
          invoke(MessageId::TrackCreditsCancel);
        });
      _steps.push_back(
        [this]
        {
          checkStored("Original title", expectedCredits("Conductor", "Anne"), 0);
          invokeDialogButton(L"PrimaryButton");
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "parent submission disables repeated Save");
          _ready = [this] { return _dialogClosed; };
        });
      _steps.push_back(
        [this]
        {
          checkStored("Composed title", expectedCredits("Staged conductor", "Staged soloist"), 1);
          releaseClosedDialog();
          invokeScope(CreditKind::Soloist);
        });
      _steps.push_back([this] { setText(singleCreditName(), L"Discarded soloist"); });
      _steps.push_back([this] { invoke(MessageId::TrackCreditsCommit); });
      _steps.push_back([this] { invokeDialogButton(L"CloseButton"); });
      _steps.push_back(
        [this]
        {
          check(_coordinatorPtr->isActive() && !_dialogClosed, "dirty native Close is cancelled for confirmation");
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "discard decision blocks Save");
          invoke(MessageId::TrackCreditsCancel);
        });
      _steps.push_back([this] { invokeScope(CreditKind::Soloist); });
      _steps.push_back(
        [this]
        {
          check(singleCreditName().Text() == L"Discarded soloist", "declining close preserves pending overlay");
          // Close with the child still active, not merely a staged parent patch.
          invokeDialogButton(L"CloseButton");
        });
      _steps.push_back(
        [this]
        {
          invoke(MessageId::AppKitDiscardChanges);
          _ready = [this] { return _dialogClosed; };
        });
      _steps.push_back(
        [this]
        {
          checkStored("Composed title", expectedCredits("Staged conductor", "Staged soloist"), 1);
          releaseClosedDialog();
          invokeScope(CreditKind::Soloist);
        });
      _steps.push_back(
        [this]
        {
          setText(singleCreditName(), L"Stale draft");
          setTitle(L"Stale title draft");
        });
      _steps.push_back([this] { mutateExternally(); });
      _steps.push_back(
        [this]
        {
          check(_externalAccepted, "external command completed with an applied revision");
          checkStored("External title", expectedCredits("Staged conductor", "Staged soloist"), 2);
          check(!singleCreditName().IsEnabled() && singleCreditName().Text() == L"Stale draft",
                "stale child is frozen without dropping its draft");
          check(!titleInput().IsEnabled() && titleInput().Text() == L"Stale title draft",
                "stale ordinary draft remains visible and disabled");
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "stale parent cannot submit");
          invoke(MessageId::TuiEditorHintReload);
        });
      _steps.push_back(
        [this]
        {
          // Both the disabled child and discard prompt have Cancel; invoke the enabled prompt action.
          for (auto const& candidate : controls<Button>())
          {
            auto text = candidate.Content().try_as<TextBlock>();

            if (candidate.IsEnabled() && text &&
                text.Text() == winrt::to_hstring(ao::i18n::requiredText(*_optCatalog, MessageId::TrackCreditsCancel)))
            {
              invokeButton(candidate);
              return;
            }
          }

          throw winrt::hresult_invalid_argument{L"Reload cancellation action is missing"};
        });
      _steps.push_back(
        [this]
        {
          check(singleCreditName().Text() == L"Stale draft" && titleInput().Text() == L"Stale title draft",
                "declining reload preserves child and ordinary drafts");
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "declining reload does not silently rebind");
          invoke(MessageId::TuiEditorHintReload);
        });
      _steps.push_back([this] { invoke(MessageId::AppKitDiscardChanges); });
      _steps.push_back(
        [this]
        {
          check(inputs(MessageId::TrackCreditName).empty(), "confirmed reload discards active child");
          check(titleInput().Text() == L"External title" && titleInput().IsEnabled(),
                "confirmed reload replaces ordinary draft from committed revision");
          check(!_workflowDialog.IsPrimaryButtonEnabled(), "reloaded unchanged form cannot submit");
          invokeScope(CreditKind::Soloist);
        });
      _steps.push_back(
        [this]
        {
          check(singleCreditName().Text() == L"Staged soloist", "reload discards stale scoped draft");
          setText(singleCreditName(), L"Reloaded soloist");
        });
      _steps.push_back([this] { invoke(MessageId::TrackCreditsCommit); });
      _steps.push_back(
        [this]
        {
          invokeDialogButton(L"PrimaryButton");
          _ready = [this] { return _dialogClosed; };
        });
      _steps.push_back(
        [this]
        {
          checkStored("External title", expectedCredits("Staged conductor", "Reloaded soloist"), 3);
          releaseClosedDialog();
        });
    }

    void planScenarios()
    {
      _steps.push_back([this] { show(emptySections(), ao::uimodel::trackCreditScope(CreditKind::Soloist)); });
      _steps.push_back(
        [this]
        {
          check(inputs(MessageId::TrackCreditName).empty(), "zero-entry category renders no invented row");
          check(button(MessageId::TrackCreditsAdd).IsTabStop(), "Add is keyboard reachable");
          invoke(MessageId::TrackCreditsAdd);
        });
      _steps.push_back(
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
      _steps.push_back(
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
      _steps.push_back(
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
      _steps.push_back([this] { invoke(MessageId::TrackCreditMoveUp, 1); });
      _steps.push_back(
        [this]
        {
          check(
            _model.entries() == std::vector<Credit>{{.name = "Anne", .kind = CreditKind::Soloist, .role = "piano"},
                                                    {.name = "Anne", .kind = CreditKind::Soloist, .role = "violin"}},
            "reorder preserves duplicate names and distinct roles");
          check(!button(MessageId::TrackCreditMoveUp, 0).IsEnabled(), "first entry cannot cross its kind boundary");
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
      _steps.push_back(
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
      _steps.push_back(
        [this]
        {
          auto kinds = controls<ComboBox>();
          check(kinds.size() == 3, "all-kind editor has row kind selectors and an Add kind selector");

          if (kinds.size() != 3)
          {
            fail("all-kind editor must expose all three kind selectors");
            return;
          }

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
      _steps.push_back(
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
          controls<ComboBox>()[0].SelectedIndex(3);
        });
      _steps.push_back(
        [this]
        {
          check(_model.entries() ==
                  std::vector<Credit>{{.name = "Existing", .kind = CreditKind::Performer, .role = "piano"},
                                      {.name = "Conductor", .kind = CreditKind::Performer, .role = "lead"}},
                "native kind change appends to destination and retains role");
          invoke(MessageId::TrackCreditDelete, 0);
        });
      _steps.push_back(
        [this]
        {
          check(_model.entries() ==
                  std::vector<Credit>{{.name = "Conductor", .kind = CreditKind::Performer, .role = "lead"}},
                "native Delete preserves remaining attributes");
          invoke(MessageId::TrackCreditsCancel);
        });
      _steps.push_back(
        [this]
        {
          check(_cancelled == 1 && !_optAccepted && !_model.isEditing(),
                "Cancel emits no replacement and ends child draft");
          auto sections = emptySections();
          sections[3] = {.mixed = true};
          show(sections, ao::uimodel::allTrackCreditKinds());
        });
      _steps.push_back(
        [this]
        {
          check(inputs(MessageId::TrackCreditName).empty(), "mixed scope never seeds target or union rows");
          check(!button(MessageId::TrackCreditsAdd).IsEnabled(), "mixed scope requires explicit intent before Add");
          check(!button(MessageId::TrackCreditsCommit).IsEnabled(), "untouched mixed scope cannot clear");
          invoke(MessageId::TrackCreditsReplaceScope);
        });
      _steps.push_back(
        [this]
        {
          check(button(MessageId::TrackCreditsAdd).IsEnabled(), "explicit replacement unlocks Add");
          check(!button(MessageId::TrackCreditsCommit).IsEnabled(), "empty mixed replacement is not an implicit clear");
          invoke(MessageId::TrackCreditsClear);
        });
      _steps.push_back(
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
      _steps.push_back(
        [this]
        {
          check(_optAccepted && _optAccepted->optCredits && _optAccepted->optCredits->kinds.all() &&
                  _optAccepted->optCredits->entries.empty(),
                "explicit mixed Clear emits exactly the full selected empty replacement");
        });
    }

    TempRoot _temp;
    std::optional<ao::i18n::MessageCatalog> _optCatalog;
    std::unique_ptr<ao::rt::AppRuntime> _runtimePtr;
    // Owned by AppRuntime, valid until completeClosing followed by runtime destruction.
    ao::winui::DispatcherQueueExecutor* _runtimeExecutor = nullptr;
    std::unique_ptr<ao::winui::TrackDetailControl> _detailPtr;
    std::unique_ptr<ao::winui::TrackPropertiesCoordinator> _coordinatorPtr;
    ao::TrackId _trackId{};
    ao::TrackId _emptyTrackId{};
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
}

int main()
{
  try
  {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Application::Start([](auto const&) { std::ignore = winrt::make<AobusCreditsProbeApplication>(); });
    return exitCode;
  }
  catch (winrt::hresult_error const& error)
  {
    std::cerr << "WinUI Credits probe startup failed: " << winrt::to_string(error.message()) << '\n';
    return 2;
  }
}

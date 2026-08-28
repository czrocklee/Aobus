// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "library/LibraryTransferCoordinator.h"

#include "pch.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/library/LibraryTransferAdapter.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.Windows.Storage.Pickers.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::winui
{
  namespace
  {
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    using namespace winrt::Microsoft::Windows::Storage::Pickers;

    constexpr double kDialogMinWidth = 520.0;
    constexpr double kDialogSpacing = 12.0;

    TextBlock wrappedText(std::string_view const value)
    {
      auto text = TextBlock{};
      text.Text(winrt::to_hstring(value));
      text.TextWrapping(TextWrapping::Wrap);
      return text;
    }

    std::string hresultMessage(winrt::hresult_error const& error)
    {
      auto message = winrt::to_string(error.message());
      return message.empty() ? "Windows reported an unknown error" : message;
    }

    void completeFireAndForgetException(std::exception_ptr const& exceptionPtr, std::string_view const context) noexcept
    {
      if (!exceptionPtr || async::isOperationCancelled(exceptionPtr))
      {
        return;
      }

      AO_FATAL_EXCEPTION(exceptionPtr, context);
    }

    template<typename ResultType, typename Finish>
    async::Task<void> finishOnCallbackExecutor(async::Runtime* const runtime,
                                               std::weak_ptr<std::monostate> lifetimePtr,
                                               async::Task<ResultType> submission,
                                               Finish finish,
                                               std::stop_token const stopToken)
    {
      auto result = co_await std::move(submission);
      co_await runtime->resumeOnCallbackExecutor(stopToken);

      if (!lifetimePtr.expired())
      {
        std::invoke(std::move(finish), std::move(result));
      }
    }
  } // namespace

  struct LibraryTransferCoordinator::Impl final
  {
    explicit Impl(LibraryTransferCoordinatorConfig config)
      : xamlRoot{std::move(config.xamlRoot)}
      , windowId{config.windowId}
      , asyncRuntime{config.asyncRuntime}
      , jobs{config.jobs}
      , notifications{config.notifications}
      , textCatalog{std::move(config.textCatalog)}
      , reportStatus{std::move(config.reportStatus)}
      , lifetimePtr{std::make_shared<std::monostate>()}
    {
    }

    void importLibrary()
    {
      if (!beginWorkflow())
      {
        return;
      }

      dialog = ContentDialog{};
      dialog.XamlRoot(xamlRoot ? xamlRoot() : XamlRoot{nullptr});
      dialog.MinWidth(kDialogMinWidth);
      dialog.Title(winrt::box_value(
        winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibrarySelectImportMode))));
      dialog.PrimaryButtonText(winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryNext)));
      dialog.CloseButtonText(winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiCommonCancel)));
      dialog.DefaultButton(ContentDialogButton::Primary);

      auto content = StackPanel{};
      content.Spacing(kDialogSpacing);
      content.Children().Append(
        wrappedText(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryChooseImportMode)));
      modeInput = ComboBox{};
      modeInput.HorizontalAlignment(HorizontalAlignment::Stretch);
      modeInput.Items().Append(winrt::box_value(
        winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryImportModeMerge))));
      modeInput.Items().Append(winrt::box_value(
        winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryImportModeRestore))));
      modeInput.SelectedIndex(0);
      content.Children().Append(modeInput);
      dialog.Content(content);
      showImportModeDialog();
    }

    void exportLibrary()
    {
      if (!beginWorkflow())
      {
        return;
      }

      dialog = ContentDialog{};
      dialog.XamlRoot(xamlRoot ? xamlRoot() : XamlRoot{nullptr});
      dialog.MinWidth(kDialogMinWidth);
      dialog.Title(winrt::box_value(
        winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibrarySelectExportMode))));
      dialog.PrimaryButtonText(winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryNext)));
      dialog.CloseButtonText(winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiCommonCancel)));
      dialog.DefaultButton(ContentDialogButton::Primary);

      auto content = StackPanel{};
      content.Spacing(kDialogSpacing);
      content.Children().Append(
        wrappedText(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryChooseBackupContents)));
      modeInput = ComboBox{};
      modeInput.Header(
        winrt::box_value(winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryInclude))));
      modeInput.HorizontalAlignment(HorizontalAlignment::Stretch);

      for (auto const id : {i18n::MessageId::WinUiLibraryExportModeDelta,
                            i18n::MessageId::WinUiLibraryExportModeMetadata,
                            i18n::MessageId::WinUiLibraryExportModeFull,
                            i18n::MessageId::WinUiLibraryExportModeListOnly})
      {
        modeInput.Items().Append(winrt::box_value(winrt::to_hstring(i18n::requiredText(textCatalog, id))));
      }

      modeInput.SelectedIndex(2);
      content.Children().Append(modeInput);
      dialog.Content(content);
      showExportModeDialog();
    }

    bool beginWorkflow() noexcept
    {
      if (retired || workflowActive)
      {
        return false;
      }

      workflowActive = true;
      return true;
    }

    winrt::fire_and_forget showImportModeDialog()
    {
      auto const weakLifetimePtr = std::weak_ptr<std::monostate>{lifetimePtr};
      auto exceptionPtr = std::exception_ptr{};

      try
      {
        auto operation = dialog.ShowAsync();
        dialogOperation = operation;
        auto const result = co_await operation;

        if (weakLifetimePtr.expired())
        {
          co_return;
        }

        auto const selection = modeInput ? modeInput.SelectedIndex() : -1;
        clearDialog();

        if (result != ContentDialogResult::Primary)
        {
          finishWorkflow();
          co_return;
        }

        auto const optMode = libraryImportModeForSelection(selection);

        if (!optMode)
        {
          finishWorkflow();
          co_return;
        }

        pickImportFile(*optMode);
      }
      catch (winrt::hresult_error const& error)
      {
        if (!weakLifetimePtr.expired())
        {
          reportNativeFailure(
            i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryImportYaml), hresultMessage(error));
        }
      }
      catch (...)
      {
        exceptionPtr = std::current_exception();
      }

      completeFireAndForgetException(exceptionPtr, "WinUI library import-mode dialog");
    }

    winrt::fire_and_forget showExportModeDialog()
    {
      auto const weakLifetimePtr = std::weak_ptr<std::monostate>{lifetimePtr};
      auto exceptionPtr = std::exception_ptr{};

      try
      {
        auto operation = dialog.ShowAsync();
        dialogOperation = operation;
        auto const result = co_await operation;

        if (weakLifetimePtr.expired())
        {
          co_return;
        }

        auto const selection = modeInput ? modeInput.SelectedIndex() : -1;
        clearDialog();

        if (result != ContentDialogResult::Primary)
        {
          finishWorkflow();
          co_return;
        }

        auto const optMode = libraryExportModeForSelection(selection);

        if (!optMode)
        {
          finishWorkflow();
          co_return;
        }

        pickExportFile(*optMode);
      }
      catch (winrt::hresult_error const& error)
      {
        if (!weakLifetimePtr.expired())
        {
          reportNativeFailure(
            i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryExportYaml), hresultMessage(error));
        }
      }
      catch (...)
      {
        exceptionPtr = std::current_exception();
      }

      completeFireAndForgetException(exceptionPtr, "WinUI library export-mode dialog");
    }

    winrt::fire_and_forget pickImportFile(rt::ImportMode const mode)
    {
      auto const weakLifetimePtr = std::weak_ptr<std::monostate>{lifetimePtr};
      auto exceptionPtr = std::exception_ptr{};

      try
      {
        auto picker = FileOpenPicker{windowId};
        picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
        picker.CommitButtonText(
          winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryImportYaml)));
        picker.FileTypeFilter().Append(L".yaml");
        picker.FileTypeFilter().Append(L".yml");
        auto operation = picker.PickSingleFileAsync();
        pickerOperation = operation;
        auto const result = co_await operation;

        if (weakLifetimePtr.expired())
        {
          co_return;
        }

        pickerOperation = nullptr;

        if (!result)
        {
          finishWorkflow();
          co_return;
        }

        startImportPreview(std::filesystem::path{result.Path().c_str()}, mode);
      }
      catch (winrt::hresult_error const& error)
      {
        if (!weakLifetimePtr.expired())
        {
          reportNativeFailure(
            i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryCouldNotSelectBackup), hresultMessage(error));
        }
      }
      catch (...)
      {
        exceptionPtr = std::current_exception();
      }

      completeFireAndForgetException(exceptionPtr, "WinUI library import file picker");
    }

    winrt::fire_and_forget pickExportFile(rt::ExportMode const mode)
    {
      auto const weakLifetimePtr = std::weak_ptr<std::monostate>{lifetimePtr};
      auto exceptionPtr = std::exception_ptr{};

      try
      {
        auto picker = FileSavePicker{windowId};
        picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
        picker.CommitButtonText(
          winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryExportYaml)));
        auto extensions = winrt::single_threaded_vector<winrt::hstring>();
        extensions.Append(L".yaml");
        extensions.Append(L".yml");
        picker.FileTypeChoices().Insert(
          winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryYamlFiles)), extensions);
        picker.DefaultFileExtension(L".yaml");
        picker.SuggestedFileName(L"aobus-library-backup");
        auto operation = picker.PickSaveFileAsync();
        pickerOperation = operation;
        auto const result = co_await operation;

        if (weakLifetimePtr.expired())
        {
          co_return;
        }

        pickerOperation = nullptr;

        if (!result)
        {
          finishWorkflow();
          co_return;
        }

        startExport(std::filesystem::path{result.Path().c_str()}, mode);
      }
      catch (winrt::hresult_error const& error)
      {
        if (!weakLifetimePtr.expired())
        {
          reportNativeFailure(i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryCouldNotSelectExportFile),
                              hresultMessage(error));
        }
      }
      catch (...)
      {
        exceptionPtr = std::current_exception();
      }

      completeFireAndForgetException(exceptionPtr, "WinUI library export file picker");
    }

    void startExport(std::filesystem::path path, rt::ExportMode const mode)
    {
      auto const weakLifetimePtr = std::weak_ptr<std::monostate>{lifetimePtr};
      asyncRuntime.spawnWithLifetime(
        tasks,
        [runtime = &asyncRuntime,
         service = &jobs,
         weakLifetimePtr,
         path = std::move(path),
         mode,
         finish = [this](Result<> result) { finishExport(std::move(result)); }](std::stop_token const stopToken) mutable
        {
          return finishOnCallbackExecutor(runtime,
                                          weakLifetimePtr,
                                          service->exportLibraryAsync(std::move(path), mode, stopToken),
                                          std::move(finish),
                                          stopToken);
        },
        "Windows library export");
    }

    void startImportPreview(std::filesystem::path path, rt::ImportMode const mode)
    {
      auto const weakLifetimePtr = std::weak_ptr<std::monostate>{lifetimePtr};
      asyncRuntime.spawnWithLifetime(
        tasks,
        [runtime = &asyncRuntime,
         service = &jobs,
         weakLifetimePtr,
         path = std::move(path),
         mode,
         finish = [this, mode](Result<rt::LibraryImportPlan> result)
         { finishImportPreview(mode, std::move(result)); }](std::stop_token const stopToken) mutable
        {
          return finishOnCallbackExecutor(runtime,
                                          weakLifetimePtr,
                                          service->prepareLibraryImportAsync(std::move(path), mode, stopToken),
                                          std::move(finish),
                                          stopToken);
        },
        "Windows library import preview");
    }

    void finishImportPreview(rt::ImportMode const mode, Result<rt::LibraryImportPlan> result)
    {
      if (!result)
      {
        reportTransferFailure(true, result.error());
        return;
      }

      if (!libraryImportRequiresDestructiveConfirmation(mode))
      {
        startImportApply(std::move(*result));
        return;
      }

      auto const preview = makeLibraryRestorePreviewState(textCatalog, result->report());
      optPendingImportPlan.emplace(std::move(*result));
      dialog = ContentDialog{};
      dialog.XamlRoot(xamlRoot ? xamlRoot() : XamlRoot{nullptr});
      dialog.MinWidth(kDialogMinWidth);
      dialog.Title(winrt::box_value(winrt::to_hstring(preview.title)));
      dialog.PrimaryButtonText(winrt::to_hstring(preview.primaryActionText));
      dialog.CloseButtonText(winrt::to_hstring(i18n::requiredText(textCatalog, i18n::MessageId::WinUiCommonCancel)));
      dialog.DefaultButton(ContentDialogButton::Close);
      dialog.Content(wrappedText(preview.message));
      showRestoreConfirmation();
    }

    winrt::fire_and_forget showRestoreConfirmation()
    {
      auto const weakLifetimePtr = std::weak_ptr<std::monostate>{lifetimePtr};
      auto exceptionPtr = std::exception_ptr{};

      try
      {
        auto operation = dialog.ShowAsync();
        dialogOperation = operation;
        auto const result = co_await operation;

        if (weakLifetimePtr.expired())
        {
          co_return;
        }

        clearDialog();

        if (result != ContentDialogResult::Primary || !optPendingImportPlan)
        {
          finishWorkflow();
          co_return;
        }

        auto plan = std::move(*optPendingImportPlan);
        optPendingImportPlan.reset();
        startImportApply(std::move(plan));
      }
      catch (winrt::hresult_error const& error)
      {
        if (!weakLifetimePtr.expired())
        {
          reportNativeFailure(
            i18n::requiredText(textCatalog, i18n::MessageId::WinUiLibraryConfirmRestore), hresultMessage(error));
        }
      }
      catch (...)
      {
        exceptionPtr = std::current_exception();
      }

      completeFireAndForgetException(exceptionPtr, "WinUI library restore-confirmation dialog");
    }

    void startImportApply(rt::LibraryImportPlan plan)
    {
      auto const weakLifetimePtr = std::weak_ptr<std::monostate>{lifetimePtr};
      asyncRuntime.spawnWithLifetime(
        tasks,
        [runtime = &asyncRuntime,
         service = &jobs,
         weakLifetimePtr,
         plan = std::move(plan),
         finish = [this](Result<rt::ImportReport> result)
         { finishImportApply(std::move(result)); }](std::stop_token const stopToken) mutable
        {
          return finishOnCallbackExecutor(runtime,
                                          weakLifetimePtr,
                                          service->applyLibraryImportPlanAsync(std::move(plan), stopToken),
                                          std::move(finish),
                                          stopToken);
        },
        "Windows library import");
    }

    void finishExport(Result<> result)
    {
      if (!result)
      {
        reportTransferFailure(false, result.error());
        return;
      }

      reportSuccess(i18n::MessageId::LibraryExported);
      finishWorkflow();
    }

    void finishImportApply(Result<rt::ImportReport> result)
    {
      if (!result)
      {
        reportTransferFailure(true, result.error());
        return;
      }

      reportSuccess(i18n::MessageId::LibraryImported);
      finishWorkflow();
    }

    void reportSuccess(i18n::MessageId const id)
    {
      auto message = std::string{i18n::requiredText(textCatalog, id)};
      notifications.post(rt::NotificationSeverity::Info, message, rt::NotificationLifetime::transient());

      if (reportStatus)
      {
        reportStatus(std::move(message));
      }
    }

    void reportTransferFailure(bool const importing, Error const& error)
    {
      APP_LOG_ERROR("Library {} failed: code={}, message={}, location={}:{}",
                    importing ? "import" : "export",
                    static_cast<int>(error.code),
                    error.message,
                    error.location.file_name(),
                    error.location.line());
      auto message =
        i18n::requiredFormat(textCatalog,
                             importing ? i18n::MessageId::LibraryImportFailed : i18n::MessageId::LibraryExportFailed,
                             {{"error", error.message}});
      notifications.post(rt::NotificationSeverity::Error, message, rt::NotificationLifetime::history());

      if (reportStatus)
      {
        reportStatus(message);
      }

      finishWorkflow();
    }

    void reportNativeFailure(std::string_view const operation, std::string detail)
    {
      APP_LOG_ERROR("Windows library file selection failed: {}: {}", operation, detail);
      auto message = i18n::requiredFormat(
        textCatalog, i18n::MessageId::WinUiLibraryFileSelectionError, {{"operation", operation}, {"message", detail}});
      notifications.post(rt::NotificationSeverity::Error, message, rt::NotificationLifetime::history());

      if (reportStatus)
      {
        reportStatus(message);
      }

      finishWorkflow();
    }

    void clearDialog() noexcept
    {
      dialogOperation = nullptr;
      modeInput = nullptr;
      dialog = nullptr;
    }

    void finishWorkflow() noexcept
    {
      optPendingImportPlan.reset();
      pickerOperation = nullptr;
      clearDialog();
      workflowActive = false;
    }

    void retire() noexcept
    {
      if (retired)
      {
        return;
      }

      retired = true;
      lifetimePtr.reset();
      tasks.cancelAll();

      if (pickerOperation)
      {
        // The callback lifetime is already expired, so native cancellation is
        // presentation-only cleanup rather than an operation invariant.
        runOptionalWinRt("cancelling the WinUI library file picker", [this] { pickerOperation.Cancel(); });
      }

      if (dialog)
      {
        runOptionalWinRt("hiding the WinUI library transfer dialog", [this] { dialog.Hide(); });
      }

      finishWorkflow();
    }

    std::function<XamlRoot()> xamlRoot;
    winrt::Microsoft::UI::WindowId windowId{};
    async::Runtime& asyncRuntime;
    rt::LibraryJobs& jobs;
    rt::NotificationService& notifications;
    i18n::MessageCatalog textCatalog;
    std::function<void(std::string)> reportStatus;
    async::LifetimeScope tasks;
    std::shared_ptr<std::monostate> lifetimePtr;
    ContentDialog dialog{nullptr};
    ComboBox modeInput{nullptr};
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> dialogOperation{nullptr};
    winrt::Windows::Foundation::IAsyncOperation<PickFileResult> pickerOperation{nullptr};
    std::optional<rt::LibraryImportPlan> optPendingImportPlan{};
    bool workflowActive = false;
    bool retired = false;
  };

  LibraryTransferCoordinator::LibraryTransferCoordinator(LibraryTransferCoordinatorConfig config)
    : _implPtr{std::make_unique<Impl>(std::move(config))}
  {
  }

  LibraryTransferCoordinator::~LibraryTransferCoordinator()
  {
    retire();
  }

  void LibraryTransferCoordinator::importLibrary()
  {
    _implPtr->importLibrary();
  }

  void LibraryTransferCoordinator::exportLibrary()
  {
    _implPtr->exportLibrary();
  }

  bool LibraryTransferCoordinator::active() const noexcept
  {
    return _implPtr->workflowActive;
  }

  void LibraryTransferCoordinator::retire() noexcept
  {
    if (_implPtr)
    {
      _implPtr->retire();
    }
  }
} // namespace ao::winui

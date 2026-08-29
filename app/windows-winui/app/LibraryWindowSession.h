// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/i18n/MessageCatalog.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace ao::rt
{
  class CompletionAliasPolicy;
  class TextOrderingPolicy;
}

namespace ao::winui
{
  class LibrarySession;

  /**
   * @brief Owns the one library-bound window graph allowed in a WinUI process.
   *
   * This owner never replaces its session. A different library is opened by
   * retiring this window, releasing this session and its runtime, and only then
   * launching a successor process. Splitting window retirement from session
   * release makes that destructive ordering explicit at the App boundary.
   */
  class [[nodiscard]] LibraryWindowSession final
  {
  public:
    using RestartRequest = compat::MoveOnlyFunction<Result<>(std::filesystem::path)>;
    using ClosedCallback = compat::MoveOnlyFunction<void()>;

    LibraryWindowSession(std::filesystem::path stateRoot,
                         winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
                         i18n::MessageCatalog textCatalog,
                         rt::TextOrderingPolicy const& textOrderingPolicy,
                         rt::CompletionAliasPolicy const& completionAliasPolicy);
    ~LibraryWindowSession();

    LibraryWindowSession(LibraryWindowSession const&) = delete;
    LibraryWindowSession& operator=(LibraryWindowSession const&) = delete;
    LibraryWindowSession(LibraryWindowSession&&) = delete;
    LibraryWindowSession& operator=(LibraryWindowSession&&) = delete;

    /// Construct and activate the process's only window and library session.
    Result<> start(std::optional<desktop::LibrarySwitchRequest> optSuccessorRequest,
                   RestartRequest requestRestart,
                   ClosedCallback onClosed);

    std::filesystem::path const& musicRoot() const noexcept;
    bool active() const noexcept;

    /// Checkpoint the active window and terminally retire playback persistence.
    Result<> prepareLibraryRestart();

    /// Retire the window, then release the session and its runtime.
    void retire() noexcept;

  private:
    void installClosedHandler(winrt::Microsoft::UI::Xaml::Window const& window);
    void handleClosed() noexcept;
    /// Retire and release the native window while retaining the session owner.
    void retireWindow() noexcept;
    void releaseSession() noexcept;

    std::filesystem::path _stateRoot;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue _dispatcher{nullptr};
    i18n::MessageCatalog _textCatalog;
    rt::TextOrderingPolicy const& _textOrderingPolicy;
    rt::CompletionAliasPolicy const& _completionAliasPolicy;
    // Declared before the window so fallback member destruction releases the
    // window first. Explicit retirement preserves the same order.
    std::unique_ptr<LibrarySession> _sessionPtr;
    winrt::Microsoft::UI::Xaml::Window _window{nullptr};
    ClosedCallback _onClosed;
    winrt::Microsoft::UI::Xaml::Window::Closed_revoker _windowClosedRevoker{};
    bool _started = false;
  };
} // namespace ao::winui

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "App.xaml.g.h"
#include <ao/Error.h>
#include <ao/desktop/LibrarySwitch.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::winui
{
  class LibraryWindowSession;
}

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::rt
{
  class CompletionAliasPolicy;
  class TextOrderingPolicy;
}

namespace winrt::Aobus::implementation
{
  struct App : AppT<App>
  {
    App();
    ~App() override;

    App(App const&) = delete;
    App& operator=(App const&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& /*args*/);

  private:
    enum class ProcessPhase : std::uint8_t
    {
      Starting,
      Running,
      RestartQueued,
      Exiting,
    };

    ao::Result<> requestLibraryRestart(std::filesystem::path root);
    void performLibraryRestart(ao::desktop::LibrarySwitchRequest request) noexcept;
    void handleWindowClosed() noexcept;
    void reportRestartLaunchFailure(ao::Error const& error) noexcept;
    void exitApplication() noexcept;

    Microsoft::UI::Dispatching::DispatcherQueue _dispatcher{nullptr};
    std::unique_ptr<ao::i18n::MessageCatalog> _messageCatalogPtr;
    std::unique_ptr<ao::rt::TextOrderingPolicy> _textOrderingPolicyPtr;
    std::unique_ptr<ao::rt::CompletionAliasPolicy> _completionAliasPolicyPtr;
    std::unique_ptr<ao::winui::LibraryWindowSession> _windowSessionPtr;
    ProcessPhase _processPhase = ProcessPhase::Starting;
  };
} // namespace winrt::Aobus::implementation

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "DesktopApplication.h"
#include <ao/Contract.h>
#include <ao/desktop/LibrarySuccessorProtocol.h>
#include <ao/utility/Path.h>

#import <AppKit/AppKit.h>
#include <unistd.h>

#include <string_view>
#include <utility>
#include <vector>

namespace
{
  void reportStartupError(std::string_view message)
  {
    std::ignore = ::write(STDERR_FILENO, message.data(), message.size());
    std::ignore = ::write(STDERR_FILENO, "\n", 1);
  }
} // namespace

int main(int argc, char** argv)
{
  @autoreleasepool
  {
    auto launch = ao::appkit::DesktopLaunch{};
    auto arguments = std::vector<std::string_view>{};

    for (std::int32_t index = 1; index < argc; ++index)
    {
      arguments.emplace_back(argv[index]);
    }

    auto parseRes = ao::desktop::parseLibrarySuccessorProtocol(arguments);

    if (!parseRes)
    {
      reportStartupError(parseRes.error().message);
      return 2;
    }

    launch.optRequest = std::move(parseRes->optRequest);
    auto const& remaining = parseRes->remainingArguments;

    for (std::size_t index = 0; index < remaining.size(); ++index)
    {
      if (remaining[index] == "--state-root" && index + 1 < remaining.size())
      {
        launch.stateRoot = std::filesystem::absolute(ao::utility::pathFromUtf8(remaining[++index]));
      }
      else if (remaining[index] == "--library" && index + 1 < remaining.size() && !launch.optRequest)
      {
        launch.optRequest = ao::desktop::LibrarySwitchRequest{
          .libraryRoot = std::filesystem::absolute(ao::utility::pathFromUtf8(remaining[++index])),
          .scanAfterOpen = true};
      }
      else
      {
        reportStartupError("Invalid arguments: expected --state-root <path> or --library <path>.");
        return 2;
      }
    }

    if (launch.stateRoot.empty())
    {
      auto* const support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                                 inDomains:NSUserDomainMask]
                              .firstObject;
      auto const* supportPath = support.fileSystemRepresentation;

      if (supportPath == nullptr)
      {
        reportStartupError("Cannot locate the user Application Support directory.");
        return 1;
      }

      launch.stateRoot = ao::utility::pathFromNative(supportPath) / "Aobus" / "macos";
    }

    auto* const executablePath = NSBundle.mainBundle.executablePath;
    AO_INVARIANT(executablePath != nil);
    launch.executable = ao::utility::pathFromNative(executablePath.fileSystemRepresentation);
    ao::appkit::disableWindowRestoration();
    auto* const app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    auto catalogRes = ao::i18n::MessageCatalog::createForSystemLocale();

    if (!catalogRes)
    {
      reportStartupError(catalogRes.error().message);
      return 1;
    }

    return ao::appkit::runDesktopApplication(std::move(launch), std::move(*catalogRes));
  }
}

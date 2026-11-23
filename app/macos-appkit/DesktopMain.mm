// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "DesktopApplication.h"
#include <ao/Contract.h>
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
    auto arguments = std::vector<std::string_view>{};

    for (std::int32_t index = 1; index < argc; ++index)
    {
      arguments.emplace_back(argv[index]);
    }

    auto startupRes = ao::appkit::parseDesktopStartupArguments(arguments);

    if (!startupRes)
    {
      reportStartupError(startupRes.error().message);
      return 2;
    }

    auto launch = std::move(startupRes->launch);

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

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitScenarioSupport.h"
#include <ao/Contract.h>
#include <ao/desktop/LibrarySuccessorProtocol.h>
#include <ao/rt/Log.h>
#include <ao/utility/Path.h>

#include <cstdio>
#include <print>
#include <string_view>
#include <utility>
#include <vector>

int main(int argc, char** argv)
{
  @autoreleasepool
  {
    auto arguments = std::vector<std::string_view>{};

    for (std::int32_t index = 1; index < argc; ++index)
    {
      arguments.emplace_back(argv[index]);
    }

    auto parsedRes = ao::desktop::parseLibrarySuccessorProtocol(arguments);

    if (!parsedRes)
    {
      return 2;
    }

    auto launch = ao::appkit::DesktopLaunch{};
    auto const successor = parsedRes->optRequest.has_value();
    launch.optRequest = std::move(parsedRes->optRequest);
    auto scenario = std::string_view{"desktop"};
    auto const& remaining = parsedRes->remainingArguments;

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
      else if (remaining[index] == "--scenario" && index + 1 < remaining.size() && !successor)
      {
        scenario = remaining[++index];
      }
      else
      {
        return 2;
      }
    }

    if (launch.stateRoot.empty() || !launch.optRequest ||
        (scenario != "desktop" && scenario != "authoring" && scenario != "presentation"))
    {
      return 2;
    }

    if (!successor)
    {
      auto fixtureRes = ao::appkit::test::prepareAppKitFixtureRoots(launch.optRequest->libraryRoot, launch.stateRoot);

      if (!fixtureRes)
      {
        std::println(stderr, "AppKit fixture preparation failed: {}", fixtureRes.error().message);
        return 2;
      }
    }

    auto* const defaults = NSUserDefaults.standardUserDefaults;
    NSMutableDictionary* argumentDomain = [[defaults volatileDomainForName:NSArgumentDomain] mutableCopy];

    if (argumentDomain == nil)
    {
      argumentDomain = [NSMutableDictionary dictionary];
    }

    argumentDomain[@"ApplePersistenceIgnoreState"] = @NO;
    argumentDomain[@"AobusRestorationProbe"] = @"preserved";
    [defaults setVolatileDomain:argumentDomain forName:NSArgumentDomain];
    ao::appkit::disableWindowRestoration();
    AO_INVARIANT([defaults boolForKey:@"ApplePersistenceIgnoreState"] != NO &&
                   [[defaults stringForKey:@"AobusRestorationProbe"] isEqualToString:@"preserved"] != NO,
                 "Production startup must disable restoration without replacing unrelated launch defaults");
    auto* const app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    auto catalogRes = ao::i18n::MessageCatalog::create("en");
    AO_INVARIANT(catalogRes);

    if (scenario == "desktop")
    {
      auto* const launcher = NSProcessInfo.processInfo.environment[@"AOBUS_APPKIT_TEST_SUCCESSOR_LAUNCHER"];
      AO_INVARIANT(launcher.length > 0, "Desktop scenarios require the process supervisor's launch bridge");
      launch.executable = ao::utility::pathFromNative(launcher.fileSystemRepresentation);
      return ao::appkit::test::runDesktopScenario(std::move(launch), std::move(*catalogRes), successor);
    }

    [app finishLaunching];
    [app activateIgnoringOtherApps:YES];
    ao::rt::Log::initialize(ao::rt::LogLevel::Info, launch.stateRoot / "logs");
    auto const result = scenario == "authoring"
                          ? ao::appkit::test::runAuthoringScenario(launch.optRequest->libraryRoot, launch.stateRoot)
                          : ao::appkit::test::runPresentationScenario(launch.optRequest->libraryRoot, launch.stateRoot);
    ao::rt::Log::shutdown();
    return result;
  }
}

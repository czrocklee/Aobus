// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitScenarioSupport.h"

#include "app/macos-appkit/AppKitText.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/Contract.h>
#include <ao/utility/Path.h>

#include <CoreFoundation/CFRunLoop.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::appkit::test
{
  namespace
  {
    constexpr auto kFixtureTrackCount = std::size_t{20};
    constexpr auto kFixtureSampleRate = std::uint32_t{44100};
    constexpr auto kFixtureDurationSeconds = std::size_t{12};

    bool containsPath(std::filesystem::path const& parent, std::filesystem::path const& child)
    {
      auto parentIt = parent.begin();
      auto childIt = child.begin();

      for (; parentIt != parent.end() && childIt != child.end(); ++parentIt, ++childIt)
      {
        if (*parentIt != *childIt)
        {
          return false;
        }
      }

      return parentIt == parent.end();
    }

    Result<std::filesystem::path> requireEmptyFixturePath(std::filesystem::path const& path, std::string_view option)
    {
      auto error = std::error_code{};
      auto const absolute = std::filesystem::absolute(path, error);

      if (error)
      {
        return makeError(
          Error::Code::IoError,
          std::format("Cannot resolve AppKit {} fixture '{}': {}", option, utility::pathToUtf8(path), error.message()));
      }

      auto resolved = std::filesystem::weakly_canonical(absolute, error);

      if (error)
      {
        return makeError(
          Error::Code::IoError,
          std::format("Cannot resolve AppKit {} fixture '{}': {}", option, utility::pathToUtf8(path), error.message()));
      }

      auto const exists = std::filesystem::exists(resolved, error);

      if (error)
      {
        return makeError(
          Error::Code::IoError,
          std::format(
            "Cannot inspect AppKit {} fixture '{}': {}", option, utility::pathToUtf8(resolved), error.message()));
      }

      if (exists)
      {
        auto const directory = std::filesystem::is_directory(resolved, error);

        if (error || !directory)
        {
          return makeError(
            Error::Code::InvalidInput,
            std::format("AppKit {} must be absent or an empty directory: {}", option, utility::pathToUtf8(resolved)));
        }

        auto const empty = std::filesystem::is_empty(resolved, error);

        if (error || !empty)
        {
          return makeError(Error::Code::InvalidInput,
                           std::format("AppKit {} must be absent or empty: {}", option, utility::pathToUtf8(resolved)));
        }
      }

      return resolved;
    }
  } // namespace

  bool tryWaitUntil(std::function<bool()> predicate, std::chrono::seconds timeout)
  {
    auto const deadline = std::chrono::steady_clock::now() + timeout;

    while (!predicate())
    {
      if (std::chrono::steady_clock::now() >= deadline)
      {
        return false;
      }

      @autoreleasepool
      {
        // Keep AppKit and the callback executor live while waiting for a semantic result.
        ::CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, static_cast<Boolean>(1));

        // Synchronous scenario waits must also keep native focus and keyboard events moving.
        while (true)
        {
          auto* const event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                 untilDate:NSDate.distantPast
                                                    inMode:NSDefaultRunLoopMode
                                                   dequeue:YES];

          if (event == nil)
          {
            break;
          }

          [NSApp sendEvent:event];
        }

        [NSApp updateWindows];
      }
    }

    return true;
  }

  void requireWaitUntil(std::function<bool()> predicate, char const* obligation, std::chrono::seconds timeout)
  {
    auto const reached = tryWaitUntil(std::move(predicate), timeout);
    AO_INVARIANT(reached, "{}", obligation);
  }

  Result<> prepareAppKitMediaLibrary(std::filesystem::path const& musicRoot)
  {
    auto rootRes = requireEmptyFixturePath(musicRoot, "--library");

    if (!rootRes)
    {
      return std::unexpected{rootRes.error()};
    }

    auto error = std::error_code{};
    std::filesystem::create_directories(*rootRes, error);

    if (error)
    {
      return makeError(
        Error::Code::IoError,
        std::format("Cannot create AppKit media fixture '{}': {}", utility::pathToUtf8(*rootRes), error.message()));
    }

    for (std::size_t index = 0; index < kFixtureTrackCount; ++index)
    {
      auto const frameCount = (kFixtureSampleRate * kFixtureDurationSeconds) + index;
      auto spec = ao::test::wav::Spec{
        .channels = 1,
        .sampleRate = kFixtureSampleRate,
        .bitsPerSample = 16,
        .audioData = std::vector<std::uint8_t>(frameCount * 2),
        .infoFields =
          {
            {.id = {'I', 'N', 'A', 'M'}, .value = std::format("AppKit Fixture Track {:02}", index + 1)},
            {.id = {'I', 'A', 'R', 'T'}, .value = "Aobus Fixture Artist"},
            {.id = {'I', 'P', 'R', 'D'}, .value = "Aobus Fixture Album"},
          },
      };
      auto const bytes = ao::test::wav::makeWav(spec);
      auto const path = *rootRes / std::format("appkit-fixture-{:02}.wav", index + 1);
      auto output = std::ofstream{path, std::ios::binary | std::ios::out | std::ios::noreplace};

      if (!output)
      {
        return makeError(
          Error::Code::IoError, std::format("Cannot create AppKit fixture track '{}'", utility::pathToUtf8(path)));
      }

      output.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      output.close();

      if (!output)
      {
        return makeError(
          Error::Code::IoError, std::format("Cannot write AppKit fixture track '{}'", utility::pathToUtf8(path)));
      }
    }

    return {};
  }

  Result<> prepareAppKitFixtureRoots(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot)
  {
    auto libraryRes = requireEmptyFixturePath(musicRoot, "--library");

    if (!libraryRes)
    {
      return std::unexpected{libraryRes.error()};
    }

    auto stateRes = requireEmptyFixturePath(stateRoot, "--state-root");

    if (!stateRes)
    {
      return std::unexpected{stateRes.error()};
    }

    if (containsPath(*libraryRes, *stateRes) || containsPath(*stateRes, *libraryRes))
    {
      return makeError(
        Error::Code::InvalidInput, "AppKit --library and --state-root must not equal or contain one another");
    }

    auto error = std::error_code{};
    std::filesystem::create_directories(*stateRes, error);

    if (error)
    {
      return makeError(
        Error::Code::IoError,
        std::format("Cannot create AppKit state fixture '{}': {}", utility::pathToUtf8(*stateRes), error.message()));
    }

    return prepareAppKitMediaLibrary(*libraryRes);
  }

  void settleNativeCallbacks()
  {
    auto const reachedNextTurnPtr = std::make_shared<bool>(false);
    ::CFRunLoopPerformBlock(::CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ *reachedNextTurnPtr = true; });
    ::CFRunLoopWakeUp(::CFRunLoopGetMain());
    requireWaitUntil([&] { return *reachedNextTurnPtr; }, "The next native callback turn must complete");
    [NSApp updateWindows];
  }

  NSView* findView(NSView* view, NSString* identifier)
  {
    if ([view.identifier isEqual:identifier] != NO || [view.accessibilityIdentifier isEqual:identifier] != NO)
    {
      return view;
    }

    auto* const children = view.subviews;

    for (NSUInteger index = 0; index < children.count; ++index)
    {
      auto* const child = children[index];

      if (auto* const found = findView(child, identifier); found != nil)
      {
        return found;
      }
    }

    return nil;
  }

  NSControl* findControl(NSView* view, NSString* identifier)
  {
    auto* const found = findView(view, identifier);
    return [found isKindOfClass:NSControl.class] != NO ? static_cast<NSControl*>(found) : nil;
  }

  void editText(NSWindow* window, NSString* identifier, NSString* text)
  {
    auto* const field = static_cast<NSTextField*>(findControl(window.contentView, identifier));
    AO_INVARIANT(field != nil, "The native editor field must exist");
    [field scrollRectToVisible:field.bounds];
    [field selectText:nil];
    auto* const editor = static_cast<NSTextView*>(field.currentEditor);
    AO_INVARIANT(editor != nil, "Text edits must go through the native field editor");
    [editor insertText:text replacementRange:NSMakeRange(0, editor.string.length)];
    AO_INVARIANT([field.stringValue isEqual:text] != 0);
  }

  void captureView(NSView* view, std::filesystem::path const& path)
  {
    [view displayIfNeeded];
    auto* const bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    AO_INVARIANT(bitmap != nil, "The native view must be drawable");
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
    auto* const data = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    auto const written = [data writeToFile:nativeText(utility::pathToUtf8(path)) atomically:YES];
    AO_INVARIANT(written != 0, "The native capture must be writable");
  }

  SessionFixture::SessionFixture(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot)
  {
    auto catalogRes = i18n::MessageCatalog::create("en");
    AO_INVARIANT(catalogRes);
    auto sessionRes =
      LibrarySession::create(musicRoot, stateRoot, false, std::move(*catalogRes), [](DesktopInvalidation) {});
    AO_INVARIANT(sessionRes, "The native fixture must acquire its isolated library");
    _sessionPtr = std::move(*sessionRes);
    _sessionPtr->rescan();
    requireWaitUntil([&] { return !_sessionPtr->state().scanning && _sessionPtr->displayIndex().rowCount() >= 3; },
                     "Native component scenarios require at least three playable fixture tracks");
  }

  SessionFixture::~SessionFixture()
  {
    requireWaitUntil([&] { return _sessionPtr->canClose(); }, "Native fixture work must settle before closing");
    _sessionPtr->close();
    _sessionPtr.reset();
  }

  LibrarySession& SessionFixture::session()
  {
    return *_sessionPtr;
  }
} // namespace ao::appkit::test

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitScenarioSupport.h"

#include "app/macos-appkit/AppKitText.h"
#include "test/audio/backend/CoreAudioTestSupport.h"
#include "test/media/wav/TestWav.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include <ao/Contract.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/utility/Path.h>

#include <CoreFoundation/CFRunLoop.h>
#import <Foundation/NSString.h>
#include <sys/unistd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <ratio>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::appkit::test
{
  namespace
  {
    constexpr std::uint32_t kFixtureSampleRate = 44100;
    static_assert(kFixtureTrackCount <= kFixtureSampleRate / std::chrono::milliseconds::period::den);

    struct FixtureLocation final
    {
      std::filesystem::path anchor;
      std::vector<std::filesystem::path> missingFromLeaf;
    };

    Result<FixtureLocation> locateFixture(std::filesystem::path const& resolved)
    {
      auto location = FixtureLocation{.anchor = resolved, .missingFromLeaf = {}};

      while (true)
      {
        auto error = std::error_code{};
        auto const status = std::filesystem::symlink_status(location.anchor, error);

        if (status.type() != std::filesystem::file_type::not_found)
        {
          if (error)
          {
            return makeError(Error::Code::IoError,
                             std::format("Cannot inspect AppKit fixture ancestor '{}': {}",
                                         utility::pathToUtf8(location.anchor),
                                         error.message()));
          }

          // weakly_canonical resolves live links. A dangling link is not a missing directory.
          if (!std::filesystem::is_directory(status))
          {
            return makeError(
              Error::Code::InvalidInput,
              std::format("AppKit fixture ancestor is not a directory: {}", utility::pathToUtf8(location.anchor)));
          }

          return location;
        }

        auto parent = location.anchor.parent_path();

        if (parent == location.anchor)
        {
          return makeError(
            Error::Code::IoError,
            std::format("Cannot locate existing AppKit fixture ancestor for '{}'", utility::pathToUtf8(resolved)));
        }

        if (location.anchor.has_filename())
        {
          location.missingFromLeaf.push_back(location.anchor.filename());
        }

        location.anchor = std::move(parent);
      }
    }

    bool isAscii(std::string_view name)
    {
      return std::ranges::all_of(name, [](unsigned char byte) { return byte <= 0x7F; });
    }

    char foldAscii(char byte)
    {
      return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A')) : byte;
    }

    Result<bool> fixtureVolumeCaseSensitive(std::filesystem::path const& anchor)
    {
      errno = 0;
      auto const caseSensitive = ::pathconf(anchor.c_str(), _PC_CASE_SENSITIVE);

      if (auto const pathconfError = errno; caseSensitive != 0 && caseSensitive != 1)
      {
        auto const reason = caseSensitive == -1 && pathconfError != 0
                              ? std::error_code{pathconfError, std::generic_category()}.message()
                              : std::format("indeterminate pathconf result ({})", caseSensitive);
        return makeError(Error::Code::IoError,
                         std::format("Cannot determine AppKit fixture volume case sensitivity at '{}': {}",
                                     utility::pathToUtf8(anchor),
                                     reason));
      }

      return caseSensitive == 1;
    }

    Result<bool> containsMissingSuffix(FixtureLocation const& parent, FixtureLocation const& child)
    {
      bool needsCaseSensitivity = false;
      bool ambiguousNonAscii = false;
      auto childPart = child.missingFromLeaf.rbegin();

      for (auto parentPart = parent.missingFromLeaf.rbegin(); parentPart != parent.missingFromLeaf.rend();
           ++parentPart, ++childPart)
      {
        if (*parentPart == *childPart)
        {
          continue;
        }

        auto const& left = parentPart->native();
        auto const& right = childPart->native();

        // A user-space Unicode table cannot prove how this volume compares missing names.
        if (!isAscii(left) || !isAscii(right))
        {
          ambiguousNonAscii = true;
          continue;
        }

        if (!std::ranges::equal(left, right, std::ranges::equal_to{}, foldAscii, foldAscii))
        {
          return false;
        }

        needsCaseSensitivity = true;
      }

      if (needsCaseSensitivity)
      {
        auto const caseSensitiveRes = fixtureVolumeCaseSensitive(parent.anchor);

        if (!caseSensitiveRes)
        {
          return std::unexpected{caseSensitiveRes.error()};
        }

        if (*caseSensitiveRes)
        {
          return false;
        }
      }

      if (ambiguousNonAscii)
      {
        return makeError(Error::Code::InvalidInput,
                         "cannot determine whether --library and --state-root overlap for missing non-ASCII names; "
                         "precreate distinct empty fixture directories");
      }

      return true;
    }

    Result<bool> containsFixturePath(FixtureLocation const& parent, FixtureLocation const& child)
    {
      if (parent.missingFromLeaf.size() > child.missingFromLeaf.size())
      {
        return false;
      }

      for (auto ancestor = child.anchor;; ancestor = ancestor.parent_path())
      {
        auto error = std::error_code{};
        auto const sameDirectory = std::filesystem::equivalent(parent.anchor, ancestor, error);

        if (error)
        {
          return makeError(Error::Code::IoError,
                           std::format("Cannot compare AppKit fixture directories '{}' and '{}': {}",
                                       utility::pathToUtf8(parent.anchor),
                                       utility::pathToUtf8(ancestor),
                                       error.message()));
        }

        if (sameDirectory)
        {
          return containsMissingSuffix(parent, child);
        }

        // A missing parent cannot contain an existing part of the child path.
        if (!parent.missingFromLeaf.empty() || ancestor == ancestor.root_path())
        {
          return false;
        }
      }
    }

    Result<> requireTraversableParentComponents(std::filesystem::path const& absolute, std::string_view option)
    {
      auto prefix = absolute.root_path();

      // weakly_canonical can remove an untraversable component followed by '..'.
      // Ask the filesystem to traverse it, not just inspect the preceding directory.
      for (auto const& part : absolute.relative_path())
      {
        prefix /= part;

        if (part == "..")
        {
          auto error = std::error_code{};

          if (auto const status = std::filesystem::status(prefix, error);
              error || !std::filesystem::is_directory(status))
          {
            return makeError(
              Error::Code::InvalidInput,
              std::format("AppKit {} fixture: '..' requires an existing directory with search permission: '{}'",
                          option,
                          utility::pathToUtf8(prefix)));
          }
        }
      }

      return {};
    }

    Result<std::filesystem::path> requireEmptyFixturePath(std::filesystem::path const& path, std::string_view option)
    {
      if ([NSString stringWithUTF8String:path.c_str()] == nil)
      {
        return makeError(Error::Code::InvalidInput, std::format("AppKit {} is not UTF-8", option));
      }

      auto error = std::error_code{};
      auto const absolute = std::filesystem::absolute(path, error);

      if (error)
      {
        return makeError(
          Error::Code::IoError,
          std::format("Cannot resolve AppKit {} fixture '{}': {}", option, utility::pathToUtf8(path), error.message()));
      }

      if (auto traversalRes = requireTraversableParentComponents(absolute, option); !traversalRes)
      {
        return std::unexpected{traversalRes.error()};
      }

      auto resolved = std::filesystem::weakly_canonical(absolute, error);

      if (error)
      {
        return makeError(
          Error::Code::IoError,
          std::format("Cannot resolve AppKit {} fixture '{}': {}", option, utility::pathToUtf8(path), error.message()));
      }

      if ([NSString stringWithUTF8String:resolved.c_str()] == nil)
      {
        return makeError(Error::Code::InvalidInput, std::format("AppKit {} resolved path is not UTF-8", option));
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

  namespace
  {
    Result<> writeFixtureTrack(std::filesystem::path const& musicRoot,
                               std::int32_t trackNumber,
                               std::chrono::milliseconds duration)
    {
      AO_EXPECTS(duration > std::chrono::milliseconds{0});
      AO_EXPECTS(trackNumber >= 1 && trackNumber <= kFixtureTrackCount);
      using FixtureFrames = std::chrono::duration<std::size_t, std::ratio<1, kFixtureSampleRate>>;
      // Start at the first frame that floors to the requested duration. The distinct per-track frame counts
      // all fit in that same millisecond.
      auto const frameCount =
        std::chrono::ceil<FixtureFrames>(duration).count() + static_cast<std::size_t>(trackNumber - 1);
      auto spec = ao::test::wav::Spec{
        .channels = 1,
        .sampleRate = kFixtureSampleRate,
        .bitsPerSample = 16,
        .audioData = std::vector<std::uint8_t>(frameCount * 2),
        .infoFields =
          {
            {.id = {'I', 'N', 'A', 'M'}, .value = std::format("AppKit Fixture Track {:02}", trackNumber)},
            {.id = {'I', 'A', 'R', 'T'}, .value = "Aobus Fixture Artist"},
            {.id = {'I', 'P', 'R', 'D'}, .value = "Aobus Fixture Album"},
          },
      };
      auto const bytes = ao::test::wav::makeWav(spec);
      auto const path = musicRoot / std::format("appkit-fixture-{:02}.wav", trackNumber);
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

      return {};
    }

    Result<> prepareMediaLibrary(std::filesystem::path const& musicRoot,
                                 std::int32_t trackCount,
                                 std::chrono::milliseconds duration,
                                 std::optional<std::chrono::milliseconds> optLastTrackDuration = std::nullopt)
    {
      AO_EXPECTS(trackCount >= 1 && trackCount <= kFixtureTrackCount);
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

      for (std::int32_t trackNumber = 1; trackNumber <= trackCount; ++trackNumber)
      {
        auto const trackDuration = trackNumber == trackCount ? optLastTrackDuration.value_or(duration) : duration;

        if (auto res = writeFixtureTrack(*rootRes, trackNumber, trackDuration); !res)
        {
          return res;
        }
      }

      return {};
    }
  } // namespace

  Result<> prepareAppKitMediaLibrary(std::filesystem::path const& musicRoot,
                                     std::chrono::milliseconds duration,
                                     std::optional<std::chrono::milliseconds> optLastTrackDuration)
  {
    return prepareMediaLibrary(musicRoot, kFixtureTrackCount, duration, optLastTrackDuration);
  }

  void preindexLibraryWithPendingTrack(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot)
  {
    if (auto res = prepareMediaLibrary(musicRoot, kIndexedFixtureTrackCount, kDefaultFixtureDuration); !res)
    {
      AO_FATAL("Could not prepare the indexed AppKit fixture: {}", res.error().message);
    }

    {
      auto fixture = SessionFixture{musicRoot, stateRoot, SessionFixture::AudioOutput::Silent};
      AO_INVARIANT(fixture.session().displayIndex().rowCount() == kIndexedFixtureTrackCount,
                   "Only the original media may be present in the saved index");
    }

    // The new media must not exist until the indexing session has released the library.
    if (auto res = writeFixtureTrack(musicRoot, kFixtureTrackCount, kDefaultFixtureDuration); !res)
    {
      AO_FATAL("Could not add the pending AppKit fixture track: {}", res.error().message);
    }
  }

  Result<> prepareAppKitFixtureRoots(std::filesystem::path& musicRoot, std::filesystem::path& stateRoot)
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

    auto const libraryLocationRes = locateFixture(*libraryRes);

    if (!libraryLocationRes)
    {
      return std::unexpected{libraryLocationRes.error()};
    }

    auto const stateLocationRes = locateFixture(*stateRes);

    if (!stateLocationRes)
    {
      return std::unexpected{stateLocationRes.error()};
    }

    auto overlapRes = containsFixturePath(*libraryLocationRes, *stateLocationRes);

    if (overlapRes && !*overlapRes)
    {
      overlapRes = containsFixturePath(*stateLocationRes, *libraryLocationRes);
    }

    if (!overlapRes)
    {
      return std::unexpected{overlapRes.error()};
    }

    if (*overlapRes)
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

    musicRoot = std::move(*libraryRes);
    stateRoot = std::move(*stateRes);
    return {};
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

  SessionFixture::SessionFixture(std::filesystem::path const& musicRoot,
                                 std::filesystem::path const& stateRoot,
                                 AudioOutput const output)
  {
    auto optOutput = std::optional<audio::Device>{};
    LibrarySession::AudioProviderFactory createProviders = audio::createPlatformBackendProviders;

    if (output == AudioOutput::Silent)
    {
      optOutput = rt::test::makeReadyAudioStatus().devices.front();
      createProviders = []
      {
        auto providers = std::vector<std::unique_ptr<audio::BackendProvider>>{};
        providers.push_back(rt::test::makeReadyAudioProvider());
        return providers;
      };
    }
    else
    {
      auto outputRes = audio::test::requestedCoreAudioTestOutput();
      auto const outputError = outputRes ? std::string{} : outputRes.error().message;
      AO_INVARIANT(outputRes, "The requested Core Audio test output must be available: {}", outputError);
      optOutput = std::move(*outputRes);
    }

    auto catalogRes = i18n::MessageCatalog::create("en");
    AO_INVARIANT(catalogRes);
    rt::LibraryCommands* commandsBorrow = nullptr;
    auto sessionRes = LibrarySession::create(
      musicRoot,
      stateRoot,
      false,
      std::move(*catalogRes),
      [](DesktopInvalidation) {},
      createProviders,
      [&commandsBorrow](rt::LibraryCommands& commands) { commandsBorrow = &commands; });
    AO_INVARIANT(sessionRes, "The native fixture must acquire its isolated library");
    _sessionPtr = std::move(*sessionRes);
    _libraryCommands = commandsBorrow;

    if (optOutput)
    {
      auto const selection = audio::OutputDeviceSelection{
        .backendId = optOutput->backendId, .deviceId = optOutput->id, .profileId = audio::kProfileShared};
      _sessionPtr->selectOutput(selection);
      requireWaitUntil(
        [&]
        {
          auto const& transport = _sessionPtr->runtime().playback().snapshot().transport;
          return transport.ready && transport.output.selectedDevice == selection &&
                 !transport.output.optPendingOutputSelection;
        },
        "The native fixture must acknowledge the exact requested output without default fallback");
      std::println("AppKit fixture test output selected: backend={} uid={} profile={}",
                   selection.backendId.raw(),
                   selection.deviceId.raw(),
                   selection.profileId.raw());
    }

    _sessionPtr->rescan();
    requireWaitUntil([&] { return !_sessionPtr->state().scanning && _sessionPtr->displayIndex().rowCount() >= 3; },
                     "Native component scenarios require at least three playable fixture tracks");
  }

  SessionFixture::~SessionFixture()
  {
    if (_sessionPtr)
    {
      requireWaitUntil([&] { return _sessionPtr->canClose(); }, "Native fixture work must settle before closing");
      close();
    }
  }

  LibrarySession& SessionFixture::session()
  {
    AO_INVARIANT(_sessionPtr);
    return *_sessionPtr;
  }

  rt::LibraryCommands& SessionFixture::libraryCommands() const noexcept
  {
    AO_INVARIANT(_sessionPtr && _libraryCommands != nullptr);
    return *_libraryCommands;
  }

  void SessionFixture::close() noexcept
  {
    _libraryCommands = nullptr;

    if (auto sessionPtr = std::exchange(_sessionPtr, nullptr))
    {
      sessionPtr->close();
    }
  }
} // namespace ao::appkit::test

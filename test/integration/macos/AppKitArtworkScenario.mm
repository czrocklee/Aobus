// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitScenarioSupport.h"
#include "app/macos-appkit/TrackInspector.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/Contract.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/utility/AtomicFile.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace
{
  std::vector<std::uint8_t> coverImage(NSColor* color, NSInteger width)
  {
    auto* const image = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                pixelsWide:width
                                                                pixelsHigh:1
                                                             bitsPerSample:8
                                                           samplesPerPixel:4
                                                                  hasAlpha:YES
                                                                  isPlanar:NO
                                                            colorSpaceName:NSDeviceRGBColorSpace
                                                               bytesPerRow:0
                                                              bitsPerPixel:0];

    for (NSInteger x = 0; x < width; ++x)
    {
      [image setColor:color atX:x y:0];
    }

    auto* const data = [image representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    AO_INVARIANT(data != nil, "Artwork regression requires a decodable image");
    auto const* const bytes = static_cast<std::uint8_t const*>(data.bytes);
    return {bytes, bytes + data.length};
  }

  void replaceArtwork(std::filesystem::path const& path, std::span<std::uint8_t const> image)
  {
    auto spec = ao::test::wav::Spec{
      .sampleRate = 44100,
      .audioData = std::vector<std::uint8_t>(std::size_t{44100} * 12 * 2),
      .infoFields = {{{.id = {'I', 'N', 'A', 'M'}, .value = "Selected artwork fixture"}}},
    };

    if (!image.empty())
    {
      spec.extraChunks.push_back({.id = {'i', 'd', '3', ' '}, .payload = ao::test::wav::makeId3WithPicture(image)});
    }

    auto const previousTime = std::filesystem::last_write_time(path);
    auto const bytes = ao::test::wav::makeWav(spec);
    // A playing decoder can retain a mapping of the old fixture. Replace the
    // file atomically instead of truncating that mapping's backing file.
    auto const publishRes =
      ao::utility::publishAtomically(path, std::string_view{reinterpret_cast<char const*>(bytes.data()), bytes.size()});
    AO_INVARIANT(publishRes, "Artwork regression must replace only its generated WAV fixture");
    // Make scan admission independent of the filesystem timestamp resolution.
    std::filesystem::last_write_time(path, previousTime + std::chrono::seconds{1});
  }
} // namespace

namespace ao::appkit::test
{
  void exerciseSelectedArtwork(LibrarySession& session, NSWindow* window, TrackId trackId)
  {
    auto const optRow = session.runtime().library().snapshot().trackRow(trackId);
    AO_INVARIANT(optRow && optRow->optUriPath, "Artwork regression requires a generated media file");
    auto const path = session.runtime().musicRoot() / *optRow->optUriPath;
    session.select({trackId});
    std::size_t selectionEvents = 0;
    auto selectionSub = session.runtime().views().onSelectionChanged([&](auto const&) { ++selectionEvents; });
    auto* const inspector = [[AobusTrackInspector alloc] initWithCatalog:session.catalog()
                                                           revealHandler:nil
                                                       propertiesHandler:nil
                                                          dismissHandler:nil];
    inspector.view.frame = NSMakeRect(0, 0, 300, 500);
    [window.contentView addSubview:inspector.view];
    NSImageView* artwork = nil;

    for (NSUInteger index = 0; index < inspector.view.subviews.count; ++index)
    {
      if (auto* const view = inspector.view.subviews[index]; [view isKindOfClass:NSImageView.class] != NO)
      {
        artwork = static_cast<NSImageView*>(view);
        break;
      }
    }

    AO_INVARIANT(artwork != nil, "Inspector must expose its artwork image");
    auto const first = coverImage([NSColor colorWithDeviceRed:1 green:0 blue:0 alpha:1], 2);
    auto const second = coverImage([NSColor colorWithDeviceRed:0 green:0 blue:1 alpha:1], 3);

    auto verify = [&](std::span<std::uint8_t const> image, CGFloat expectedWidth)
    {
      replaceArtwork(path, image);
      session.rescan();
      requireWaitUntil([&] { return !session.state().scanning; }, "Artwork rescan must finish");
      requireWaitUntil([&] { return std::ranges::equal(session.state().selectedCover.view(), std::as_bytes(image)); },
                       "Selected artwork must follow committed resource changes without a selection event");
      AO_INVARIANT(session.selection() == std::vector{trackId} && selectionEvents == 0,
                   "Artwork refresh must preserve the selected identity without synthesizing selection events");
      [inspector renderArtwork:session.state().selectedCover];

      if (image.empty())
      {
        AO_INVARIANT(artwork.image == nil, "Removing artwork must clear the inspector image");
      }
      else
      {
        AO_INVARIANT(artwork.image != nil && artwork.image.size.width == expectedWidth,
                     "Inspector must decode the replacement resource rather than retain its previous image");
      }
    };

    verify(first, 2);
    // Remembered selection remains meaningful when filtering hides its row.
    session.filter("$title = \"No such artwork fixture\"");
    requireWaitUntil([&] { return session.displayIndex().rowCount() == 0; }, "Filter must hide the selected track");
    verify(second, 3);
    verify({}, 0);
    verify(first, 2);
    session.filter("");
    requireWaitUntil(
      [&] { return session.displayIndexOf(trackId).has_value(); }, "Clearing the filter must reveal selection");
    [inspector detach];
    [inspector.view removeFromSuperview];
  }
} // namespace ao::appkit::test

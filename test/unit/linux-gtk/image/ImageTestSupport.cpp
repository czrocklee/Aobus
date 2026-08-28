// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/linux-gtk/image/ImageTestSupport.h"

#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/resource/ResourceDiskCache.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>
#include <gdkmm/pixbuf.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace ao::gtk::test
{
  Glib::RefPtr<Gdk::Pixbuf> makePixbuf(std::int32_t const width, std::int32_t const height)
  {
    return Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, width, height);
  }

  Glib::RefPtr<Gdk::Pixbuf> makePixbuf(std::int32_t const side)
  {
    return makePixbuf(side, side);
  }

  ResourceId writeRawResource(library::MusicLibrary& library, std::span<std::byte const> const bytes)
  {
    auto transaction = library::test::writeTransaction(library);
    auto idRes = library::test::physicalWriter(library.resources(), transaction).create(bytes);
    REQUIRE(idRes);
    auto const id = *idRes;
    REQUIRE(transaction.commit());
    return id;
  }

  std::vector<std::byte> encodePng(Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
  {
    gchar* rawBuffer = nullptr;
    gsize bufferSize = 0;
    pixbufPtr->save_to_buffer(rawBuffer, bufferSize, "png");
    auto bufferPtr = std::unique_ptr<gchar, decltype(&::g_free)>{rawBuffer, &::g_free};

    auto const bytes = std::span<std::byte const>{
      reinterpret_cast<std::byte const*>(bufferPtr.get()), static_cast<std::size_t>(bufferSize)};
    return {bytes.begin(), bytes.end()};
  }

  ResourceId writeCoverResource(library::MusicLibrary& library, Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
  {
    return writeRawResource(library, encodePng(pixbufPtr));
  }

  ResourceId writeCoverResource(library::MusicLibrary& library, std::int32_t const side)
  {
    return writeCoverResource(library, makePixbuf(side));
  }

  void installCoverCacheEntry(std::filesystem::path const& cacheDirectory, std::span<std::byte const> const bytes)
  {
    auto const cache = rt::ResourceDiskCache{rt::ResourceDiskCache::Config{
      .directory = rt::coverCacheDirectory(cacheDirectory),
      .maximumEntryBytes = rt::LibraryJobs::kMaximumInteractiveResourceBytes,
    }};
    cache.store(utility::computeSha256(bytes), bytes);
    REQUIRE(cache.read(utility::computeSha256(bytes)));
  }
} // namespace ao::gtk::test

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <glibmm/refptr.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace Gdk
{
  class Pixbuf;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::gtk::test
{
  Glib::RefPtr<Gdk::Pixbuf> makePixbuf(std::int32_t width, std::int32_t height);

  Glib::RefPtr<Gdk::Pixbuf> makePixbuf(std::int32_t side);

  ResourceId writeRawResource(library::MusicLibrary& library, std::span<std::byte const> bytes);

  std::vector<std::byte> encodePng(Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr);

  ResourceId writeCoverResource(library::MusicLibrary& library, Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr);

  ResourceId writeCoverResource(library::MusicLibrary& library, std::int32_t side);

  /**
   * @brief Installs @p bytes in the cover cache under @p cacheDirectory.
   *
   * A resource row names content and holds none, so a runtime request finds
   * bytes through the derived cover cache or through an audio file that carries
   * them. Synthetic test imagery has no carrier, so the cache is its source; the
   * entry is installed through the production cache so the test depends on the
   * real layout and verification rather than a hand-built path.
   */
  void installCoverCacheEntry(std::filesystem::path const& cacheDirectory, std::span<std::byte const> bytes);
} // namespace ao::gtk::test

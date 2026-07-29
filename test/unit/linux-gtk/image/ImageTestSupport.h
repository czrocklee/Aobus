// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <glibmm/refptr.h>

#include <cstddef>
#include <cstdint>
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
} // namespace ao::gtk::test

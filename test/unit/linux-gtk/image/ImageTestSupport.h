// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>

#include <catch2/catch_test_macros.hpp>
#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace ao::gtk::test
{
  inline Glib::RefPtr<Gdk::Pixbuf> makePixbuf(std::int32_t width, std::int32_t height)
  {
    return Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, width, height);
  }

  inline Glib::RefPtr<Gdk::Pixbuf> makePixbuf(std::int32_t side)
  {
    return makePixbuf(side, side);
  }

  inline ResourceId writeRawResource(library::MusicLibrary& library, std::span<std::byte const> bytes)
  {
    auto txn = library.writeTransaction();
    auto idResult = library.resources().writer(txn).create(bytes);
    REQUIRE(idResult);
    auto const id = *idResult;
    REQUIRE(txn.commit());
    return id;
  }

  inline ResourceId writeCoverResource(library::MusicLibrary& library, Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
  {
    gchar* rawBuffer = nullptr;
    gsize bufferSize = 0;
    pixbufPtr->save_to_buffer(rawBuffer, bufferSize, "png");
    auto bufferPtr = std::unique_ptr<gchar, decltype(&::g_free)>{rawBuffer, &::g_free};

    auto const bytes = std::span<std::byte const>{
      reinterpret_cast<std::byte const*>(bufferPtr.get()), static_cast<std::size_t>(bufferSize)};
    return writeRawResource(library, bytes);
  }

  inline ResourceId writeCoverResource(library::MusicLibrary& library, std::int32_t side)
  {
    return writeCoverResource(library, makePixbuf(side));
  }

  inline bool pumpUntil(std::function<bool()> const& predicate,
                        std::chrono::milliseconds timeout = std::chrono::seconds{5})
  {
    return pumpGtkEventsUntil(predicate, timeout);
  }
} // namespace ao::gtk::test

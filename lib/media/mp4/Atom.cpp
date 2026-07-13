// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "media/mp4/AtomDispatch.h"
#include <ao/Error.h>
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/AtomLayout.h>
#include <ao/utility/ByteView.h>

#include <boost/endian/buffers.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace ao::media::mp4
{
  static_assert(std::is_trivially_copyable_v<AtomView>);
  static_assert(std::is_trivially_copyable_v<AtomCursor>);

  namespace
  {
    constexpr std::size_t kAudioSampleEntryHeaderSkip = 28;
    constexpr std::size_t kAtomHeaderSize = sizeof(AtomLayout);

    struct ExtendedAtomLayout final
    {
      AtomLayout common;
      boost::endian::big_uint64_buf_t length;
    };

    static_assert(sizeof(ExtendedAtomLayout) == 16);
    static_assert(alignof(ExtendedAtomLayout) == 1);
    static_assert(utility::layout::kIsBinaryLayoutType<ExtendedAtomLayout>);

    std::optional<std::size_t> childHeaderSkip(std::string_view parentType, std::string_view type)
    {
      if (auto const* entry = ContainerAtomDispatchTable::lookupContainerAtom(type.data(), type.size());
          entry != nullptr)
      {
        return entry->skipSize;
      }

      if (parentType == "stsd" && (type == "alac" || type == "mp4a"))
      {
        return kAudioSampleEntryHeaderSkip;
      }

      return std::nullopt;
    }
  } // namespace

  AtomView AtomView::root(std::span<std::byte const> bytes) noexcept
  {
    return AtomView{bytes, bytes, "root", 0, true};
  }

  AtomCursor AtomView::children() const noexcept
  {
    return AtomCursor{_childBytes, _type};
  }

  Result<OptionalAtom> AtomCursor::next()
  {
    if (_remaining.empty())
    {
      return OptionalAtom{};
    }

    if (_remaining.size() < kAtomHeaderSize)
    {
      return makeError(Error::Code::CorruptData, "mp4 atom header is truncated");
    }

    auto const* const layout = utility::layout::view<AtomLayout>(_remaining);
    auto const compactLength = layout->length.value();
    std::size_t headerSize = kAtomHeaderSize;
    std::size_t length = 0;

    if (compactLength == 0)
    {
      if (_parentType != "root")
      {
        return makeError(Error::Code::FormatRejected, "mp4 end-of-file atom size is only valid at file root");
      }

      length = _remaining.size();
    }
    else if (compactLength == 1)
    {
      if (_remaining.size() < sizeof(ExtendedAtomLayout))
      {
        return makeError(Error::Code::CorruptData, "mp4 extended atom header is truncated");
      }

      auto const* const extendedLayout = utility::layout::view<ExtendedAtomLayout>(_remaining);
      auto const extendedLength = extendedLayout->length.value();

      if (extendedLength < sizeof(ExtendedAtomLayout))
      {
        return makeError(Error::Code::CorruptData, "mp4 extended atom length is smaller than its header");
      }

      if (extendedLength > _remaining.size())
      {
        return makeError(Error::Code::CorruptData, "mp4 extended atom size exceeds its container boundary");
      }

      headerSize = sizeof(ExtendedAtomLayout);
      length = static_cast<std::size_t>(extendedLength);
    }
    else
    {
      length = static_cast<std::size_t>(compactLength);
    }

    if (length < kAtomHeaderSize)
    {
      return makeError(Error::Code::CorruptData, "mp4 atom length is smaller than its header");
    }

    if (length > _remaining.size())
    {
      return makeError(Error::Code::CorruptData, "mp4 atom size exceeds its container boundary");
    }

    auto const atomBytes = _remaining.first(length);
    auto const type = utility::bytes::stringView(utility::bytes::view(layout->type));
    auto childBytes = std::span<std::byte const>{};
    bool isContainer = false;

    if (auto const optSkip = childHeaderSkip(_parentType, type); optSkip && length >= headerSize + *optSkip)
    {
      childBytes = atomBytes.subspan(headerSize + *optSkip);
      isContainer = true;
    }

    _remaining = _remaining.subspan(length);
    auto atom = AtomView{atomBytes, childBytes, type, headerSize, isContainer};
    return OptionalAtom{atom};
  }

  AtomView fromBuffer(std::span<std::byte const> data) noexcept
  {
    return AtomView::root(data);
  }

  Result<OptionalAtom> findAtom(AtomView const& root, std::span<std::string_view const> path)
  {
    if (path.empty() || path.front() != root.type())
    {
      return OptionalAtom{};
    }

    if (path.size() == 1)
    {
      return OptionalAtom{root};
    }

    auto cursor = root.children();

    while (true)
    {
      auto childResult = cursor.next();

      if (!childResult)
      {
        return std::unexpected{childResult.error()};
      }

      if (!*childResult)
      {
        return OptionalAtom{};
      }

      if (auto const& child = **childResult; child.type() == path[1])
      {
        auto foundResult = findAtom(child, path.subspan(1));

        if (!foundResult)
        {
          return std::unexpected{foundResult.error()};
        }

        if (*foundResult)
        {
          return *foundResult;
        }
      }
    }
  }
} // namespace ao::media::mp4

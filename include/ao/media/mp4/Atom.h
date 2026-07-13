// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "AtomLayout.h"
#include <ao/Error.h>
#include <ao/utility/ByteView.h>

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace ao::media::mp4
{
  class AtomCursor;

  /**
   * Non-owning view of one MP4 atom or of the synthetic root container.
   *
   * Views contain only spans into the caller-owned file buffer. Copying a view
   * does not parse children, allocate nodes, or extend the buffer lifetime.
   */
  class AtomView final
  {
  public:
    static AtomView root(std::span<std::byte const> bytes) noexcept;

    AtomView(AtomView const&) = default;
    AtomView& operator=(AtomView const&) = default;
    AtomView(AtomView&&) noexcept = default;
    AtomView& operator=(AtomView&&) noexcept = default;
    ~AtomView() = default;

    std::string_view type() const noexcept { return _type; }
    std::span<std::byte const> bytes() const noexcept { return _bytes; }
    std::span<std::byte const> payload() const noexcept { return _bytes.subspan(_headerSize); }
    bool isLeaf() const noexcept { return !_isContainer; }

    AtomCursor children() const noexcept;

    /**
     * Returns a typed view for layouts that begin with a compact AtomLayout,
     * only when both the available bytes and declared length satisfy Layout.
     * Extended-size semantic readers parse payload() relative to the actual
     * header instead. Malformed file data is an ordinary parse failure.
     */
    template<typename Layout>
    Layout const* tryLayout() const noexcept
    {
      auto const* const atomLayout = utility::bytes::tryLayout<AtomLayout>(_bytes);
      auto const* const typedLayout = utility::bytes::tryLayout<Layout>(_bytes);

      if (_headerSize != sizeof(AtomLayout) || atomLayout == nullptr || typedLayout == nullptr)
      {
        return nullptr;
      }

      auto const declaredLength = static_cast<std::size_t>(atomLayout->length.value());

      if (declaredLength > _bytes.size())
      {
        return nullptr;
      }

      if constexpr (Layout::FixedSize::value)
      {
        return declaredLength == sizeof(Layout) ? typedLayout : nullptr;
      }

      return declaredLength >= sizeof(Layout) ? typedLayout : nullptr;
    }

  private:
    friend class AtomCursor;

    AtomView(std::span<std::byte const> bytes,
             std::span<std::byte const> childBytes,
             std::string_view type,
             std::size_t headerSize,
             bool isContainer) noexcept
      : _bytes{bytes}, _childBytes{childBytes}, _type{type}, _headerSize{headerSize}, _isContainer{isContainer}
    {
    }

    std::span<std::byte const> _bytes;
    std::span<std::byte const> _childBytes;
    std::string_view _type;
    std::size_t _headerSize = 0;
    bool _isContainer = false;
  };

  using OptionalAtom = std::optional<AtomView>;

  /**
   * Single-pass cursor over the immediate children of an AtomView.
   *
   * next() validates the complete boundary of each returned child. It reports
   * malformed trailing bytes when the caller advances to them; callers that
   * stop early deliberately stop validation at the same point.
   */
  class AtomCursor final
  {
  public:
    Result<OptionalAtom> next();

  private:
    friend class AtomView;

    AtomCursor(std::span<std::byte const> remaining, std::string_view parentType) noexcept
      : _remaining{remaining}, _parentType{parentType}
    {
    }

    std::span<std::byte const> _remaining;
    std::string_view _parentType;
  };

  AtomView fromBuffer(std::span<std::byte const> data) noexcept;

  /**
   * Finds the first atom at path, validating sibling boundaries only until a
   * complete match is found.
   * The first path element names root itself (for example root/moov/trak).
   */
  Result<OptionalAtom> findAtom(AtomView const& root, std::span<std::string_view const> path);

  template<typename Visitor>
  Result<> visitChildren(AtomView const& parent, Visitor visitor)
  {
    auto cursor = parent.children();

    while (true)
    {
      auto childResult = cursor.next();

      if (!childResult)
      {
        return std::unexpected{childResult.error()};
      }

      if (!*childResult)
      {
        return {};
      }

      if (!std::invoke(visitor, **childResult))
      {
        return {};
      }
    }
  }
} // namespace ao::media::mp4

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <cstddef>
#include <utility>

namespace ao::uimodel
{
  struct LayoutDocument;

  struct LayoutTreeLimits final
  {
    std::size_t maxEntries = 0;
    std::size_t maxDepth = 0;
    std::size_t maxValueBytes = 0;
  };

  struct LayoutDocumentLimits final
  {
    static constexpr std::size_t kDefaultMaxFileBytes = std::size_t{256} * 1024;
    static constexpr std::size_t kDefaultMaxAuthoredEntries = 4096;
    static constexpr std::size_t kDefaultMaxEffectiveEntries = 2048;
    static constexpr std::size_t kDefaultMaxDepth = 64;
    static constexpr std::size_t kDefaultMaxAuthoredValueBytes = std::size_t{256} * 1024;
    static constexpr std::size_t kDefaultMaxEffectiveValueBytes = std::size_t{512} * 1024;

    std::size_t maxFileBytes = kDefaultMaxFileBytes;
    LayoutTreeLimits authored{.maxEntries = kDefaultMaxAuthoredEntries,
                              .maxDepth = kDefaultMaxDepth,
                              .maxValueBytes = kDefaultMaxAuthoredValueBytes};
    LayoutTreeLimits effective{.maxEntries = kDefaultMaxEffectiveEntries,
                               .maxDepth = kDefaultMaxDepth,
                               .maxValueBytes = kDefaultMaxEffectiveValueBytes};
  };

  class PreparedLayout final
  {
  public:
    PreparedLayout(PreparedLayout const&) = default;
    PreparedLayout& operator=(PreparedLayout const&) = default;
    PreparedLayout(PreparedLayout&&) noexcept = default;
    PreparedLayout& operator=(PreparedLayout&&) noexcept = default;
    ~PreparedLayout() = default;

    LayoutNode const& effectiveRoot() const noexcept { return _effectiveRoot; }

  private:
    explicit PreparedLayout(LayoutNode effectiveRoot)
      : _effectiveRoot{std::move(effectiveRoot)}
    {
    }

    friend Result<PreparedLayout> prepareLayout(LayoutDocument const& document, LayoutDocumentLimits const& limits);

    LayoutNode _effectiveRoot;
  };

  Result<PreparedLayout> prepareLayout(LayoutDocument const& document,
                                       LayoutDocumentLimits const& limits = LayoutDocumentLimits{});
} // namespace ao::uimodel

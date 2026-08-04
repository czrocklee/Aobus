// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  struct LayoutNode;
  struct LayoutValue;

  /// Why a layout document was rejected as a whole.
  enum class LayoutRejectionReason : std::uint8_t
  {
    UnknownComponentType,
    UnsupportedLayoutField,
    InvalidLayoutFieldValue,
    UnknownProperty,
    InvalidPropertyValue,
    ChildCountBelowMinimum,
    ChildCountAboveMaximum,
    MissingRequiredId,
    DuplicateNodeId,
    UnsupportedActionSlot,
    UnknownAction,
    UnsupportedSurface,
  };

  /// What a dialect says about a layout field beyond the common vocabulary.
  enum class LayoutFieldRuling : std::uint8_t
  {
    /// The dialect does not name the field; the component descriptor decides.
    Unclaimed,
    /// The dialect owns the field and the authored value is well formed.
    Accepted,
    /// The dialect owns the field and its value, or its use here, is a defect.
    Rejected,
  };

  struct LayoutFieldVerdict final
  {
    LayoutFieldRuling ruling = LayoutFieldRuling::Unclaimed;
    LayoutRejectionReason reason = LayoutRejectionReason::UnsupportedLayoutField;
    std::string message = {};

    static LayoutFieldVerdict unclaimed() { return {}; }
    static LayoutFieldVerdict accepted() { return {.ruling = LayoutFieldRuling::Accepted}; }

    static LayoutFieldVerdict rejected(LayoutRejectionReason const reason, std::string message)
    {
      return {.ruling = LayoutFieldRuling::Rejected, .reason = reason, .message = std::move(message)};
    }
  };

  /**
   * @brief What one frontend adds to the shared document rules.
   *
   * The document, the common layout fields, the component catalog, and the
   * action catalog are the same wherever a shell is built, so the traversal that
   * checks them is written once. What differs is narrow and named here: the
   * styling vocabulary a frontend understands, whether it hosts authored
   * tooltips, and the few component types whose authored presentation tightens
   * their own rules.
   *
   * A null hook means the dialect has no opinion, which is what an unset
   * function reads as at every call site.
   */
  struct LayoutDialect final
  {
    /// How the frontend names itself in a rejection message.
    std::string_view name = "layout";

    /**
     * @brief Verdict for a layout field the common vocabulary does not name.
     *
     * Consulted before the component descriptor, so a dialect can reject
     * another frontend's styling field even where a descriptor would accept it.
     */
    LayoutFieldVerdict (*layoutField)(LayoutNode const& node,
                                      std::string_view name,
                                      LayoutValue const& value) = nullptr;

    /**
     * @brief Exact child count @p node's authored presentation requires, if any.
     *
     * A presentation that swaps the constructed element also decides whether the
     * component hosts a content region, which the descriptor's child range alone
     * cannot express.
     */
    std::optional<std::size_t> (*presentationChildCount)(LayoutNode const& node) = nullptr;

    /**
     * @brief Whether a component type must carry a stable authored id.
     *
     * A shell that destroys and rebuilds every element needs the components
     * whose semantic state it reconciles to be locatable by id rather than by
     * position in the tree.
     */
    bool (*requiresStableId)(std::string_view type) = nullptr;

    /// Whether the frontend builds the tooltip a node authors, rather than owning tooltips per component.
    bool authorsTooltips = false;
  };
} // namespace ao::uimodel

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  /**
   * @brief A component concept more than one shell presents.
   *
   * A shell that presents one of these builds its descriptor from
   * `sharedComponentDescriptor` rather than spelling the type name and the
   * shared properties again. Naming the concept as an enumerator is what makes
   * the duplication impossible: there is one place a type string is written, so
   * two shells cannot end up calling the same component `track.table` and
   * `tracks.table`, nor give one component's `variant` two different meanings.
   *
   * Membership is a claim about the concept, not about coverage. A shell picks
   * the subset it implements and adds whatever its toolkit alone can honor
   * through `withShellProperties`; nothing here obliges a shell to present a
   * component it has no widget for.
   */
  enum class SharedLayoutComponentType : std::uint8_t
  {
    Box,
    Split,
    Label,
    ActionButton,
    MenuButton,
    MenuBar,
    TrackTable,
    TrackQuickFilter,
    TrackPresentationButton,
    TrackCoverArt,
    PlaybackTransportButton,
    PlaybackSoulButton,
    PlaybackSeekSlider,
    PlaybackTimeLabel,
    PlaybackVolumeControl,
    PlaybackOutputDeviceSelector,
    StatusActivity,
    StatusTrackCount,
    StatusSelectionInfo,
    StatusMessage,
  };

  /**
   * @name Shared property names
   *
   * A property listed here means the same thing in every shell that registers
   * the component carrying it, so a document moves its authored value between
   * shells unchanged. A shell reads them through these constants for the same
   * reason it builds descriptors from the enumerators: the string is written
   * once.
   * @{
   */
  inline constexpr auto kOrientationProp = std::string_view{"orientation"};
  inline constexpr auto kSpacingProp = std::string_view{"spacing"};
  inline constexpr auto kTextProp = std::string_view{"text"};
  inline constexpr auto kVariantProp = std::string_view{"variant"};
  inline constexpr auto kPlaceholderStyleProp = std::string_view{"placeholderStyle"};
  inline constexpr auto kStrokeWidthProp = std::string_view{"strokeWidth"};
  inline constexpr auto kGlyphScaleProp = std::string_view{"glyphScale"};
  inline constexpr auto kModeProp = std::string_view{"mode"};
  inline constexpr auto kCommandProp = std::string_view{"command"};
  /// @}

  /// Every shared component, in enumerator order.
  std::vector<SharedLayoutComponentType> sharedLayoutComponentTypes();

  /**
   * @brief The type id documents author for @p component.
   *
   * A structural or generic primitive carries no domain prefix; everything that
   * names part of the application's subject matter does.
   */
  constexpr std::string_view componentTypeName(SharedLayoutComponentType const component) noexcept
  {
    switch (component)
    {
      case SharedLayoutComponentType::Box: return "box";
      case SharedLayoutComponentType::Split: return "split";
      case SharedLayoutComponentType::Label: return "label";
      case SharedLayoutComponentType::ActionButton: return "actionButton";
      case SharedLayoutComponentType::MenuButton: return "menuButton";
      case SharedLayoutComponentType::MenuBar: return "app.menuBar";
      case SharedLayoutComponentType::TrackTable: return "track.table";
      case SharedLayoutComponentType::TrackQuickFilter: return "track.quickFilter";
      case SharedLayoutComponentType::TrackPresentationButton: return "track.presentationButton";
      case SharedLayoutComponentType::TrackCoverArt: return "track.coverArt";
      case SharedLayoutComponentType::PlaybackTransportButton: return "playback.transportButton";
      case SharedLayoutComponentType::PlaybackSoulButton: return "playback.soulButton";
      case SharedLayoutComponentType::PlaybackSeekSlider: return "playback.seekSlider";
      case SharedLayoutComponentType::PlaybackTimeLabel: return "playback.timeLabel";
      case SharedLayoutComponentType::PlaybackVolumeControl: return "playback.volumeControl";
      case SharedLayoutComponentType::PlaybackOutputDeviceSelector: return "playback.outputDeviceSelector";
      case SharedLayoutComponentType::StatusActivity: return "status.activity";
      case SharedLayoutComponentType::StatusTrackCount: return "status.trackCount";
      case SharedLayoutComponentType::StatusSelectionInfo: return "status.selectionInfo";
      case SharedLayoutComponentType::StatusMessage: return "status.message";
    }

    return {};
  }

  /// The shared component @p type names, or nullopt when @p type is a shell's own.
  std::optional<SharedLayoutComponentType> sharedComponentFor(std::string_view type) noexcept;

  /**
   * @brief The descriptor every shell presenting @p component starts from.
   *
   * Carries the type id, the display name, the category, the child range, the
   * action policy, and only those properties whose authored value means the
   * same thing everywhere. A shell adds its own on top; it never restates what
   * is already here.
   */
  LayoutComponentDescriptor sharedComponentDescriptor(SharedLayoutComponentType component);

  /**
   * @brief @p descriptor with @p properties, which only the calling shell honors.
   *
   * Contract: no entry of @p properties renames or redefines a shared property.
   * A shell that reaches for a name the vocabulary already spent is describing
   * the shared concept differently rather than extending it, which is the
   * divergence this vocabulary exists to prevent.
   */
  /**
   * @brief @p descriptor with the action slots @p policy adds.
   *
   * The shared slot set is a floor: every slot the vocabulary names must be
   * bindable in every shell that registers the type. A shell whose toolkit
   * carries a gesture the others lack widens the set here, and may name its own
   * default action for any slot.
   *
   * Narrowing is rejected, because a document that binds a shared slot must
   * work everywhere the type is registered. A shell that cannot serve a slot
   * does not register the type.
   */
  LayoutComponentDescriptor withShellActionSlots(LayoutComponentDescriptor descriptor,
                                                 LayoutComponentActionPolicy const& policy);

  LayoutComponentDescriptor withShellProperties(LayoutComponentDescriptor descriptor,
                                                std::vector<LayoutPropertyDescriptor> properties);

  /// As `withShellProperties`, for properties the parent applies to the child's placement.
  LayoutComponentDescriptor withShellLayoutProperties(LayoutComponentDescriptor descriptor,
                                                      std::vector<LayoutPropertyDescriptor> properties);

  /**
   * @brief Every way @p catalog describes a shared component differently from the vocabulary.
   *
   * Reports one human-readable line per departure and an empty vector when the
   * catalog agrees. A shell's own components are ignored, as are properties the
   * shell added; what is checked is that a shared type keeps its shared meaning.
   *
   * This exists because a descriptor can still be built by hand. Both shells
   * run it over their live catalog, so a hand-built descriptor that drifts is
   * caught where the catalog is assembled rather than by a reader comparing two
   * frontends.
   */
  std::vector<std::string> sharedVocabularyDepartures(LayoutComponentCatalog const& catalog);
} // namespace ao::uimodel

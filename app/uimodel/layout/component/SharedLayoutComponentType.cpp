// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>

#include <ao/Contract.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    constexpr auto kDefaultGlyphScale = 1.0;

    LayoutPropertyDescriptor stringProp(std::string_view const name, std::string_view const label)
    {
      return {.name = std::string{name},
              .kind = LayoutPropertyKind::String,
              .label = std::string{label},
              .defaultValue = LayoutValue{std::string{}}};
    }

    LayoutPropertyDescriptor enumProp(std::string_view const name,
                                      std::string_view const label,
                                      std::vector<std::string> values,
                                      std::string_view const defaultValue)
    {
      return {.name = std::string{name},
              .kind = LayoutPropertyKind::Enum,
              .label = std::string{label},
              .defaultValue = LayoutValue{std::string{defaultValue}},
              .enumValues = std::move(values)};
    }

    LayoutPropertyDescriptor orientationProp()
    {
      return enumProp(kOrientationProp, "Orientation", {"vertical", "horizontal"}, "vertical");
    }

    LayoutPropertyDescriptor scalarProp(std::string_view const name,
                                        std::string_view const label,
                                        LayoutPropertyKind const kind,
                                        LayoutValue defaultValue)
    {
      return {
        .name = std::string{name}, .kind = kind, .label = std::string{label}, .defaultValue = std::move(defaultValue)};
    }

    /// A component that draws itself and hosts nothing.
    LayoutComponentDescriptor leaf(std::string_view const type,
                                   std::string_view const displayName,
                                   LayoutComponentCategory const category,
                                   std::vector<LayoutPropertyDescriptor> props = {})
    {
      return {.type = std::string{type},
              .displayName = std::string{displayName},
              .category = category,
              .props = std::move(props),
              .optMaxChildren = 0};
    }

    LayoutPropertyDescriptor const* findProperty(std::vector<LayoutPropertyDescriptor> const& props,
                                                 std::string_view const name)
    {
      auto const it = std::ranges::find(props, name, &LayoutPropertyDescriptor::name);
      return it == props.end() ? nullptr : std::addressof(*it);
    }

    bool describesSameProperty(LayoutPropertyDescriptor const& shared, LayoutPropertyDescriptor const& candidate)
    {
      // The label is presentation for an editor and may be worded per shell;
      // everything a document's author sees is compared.
      return shared.kind == candidate.kind && shared.defaultValue.data == candidate.defaultValue.data &&
             shared.enumValues == candidate.enumValues;
    }

    std::string describeChildRange(LayoutComponentDescriptor const& descriptor)
    {
      return descriptor.optMaxChildren ? std::format("{}..{}", descriptor.minChildren, *descriptor.optMaxChildren)
                                       : std::format("{}..any", descriptor.minChildren);
    }
  } // namespace

  std::vector<SharedLayoutComponentType> sharedLayoutComponentTypes()
  {
    return {SharedLayoutComponentType::Box,
            SharedLayoutComponentType::Split,
            SharedLayoutComponentType::Label,
            SharedLayoutComponentType::ActionButton,
            SharedLayoutComponentType::MenuButton,
            SharedLayoutComponentType::MenuBar,
            SharedLayoutComponentType::TrackTable,
            SharedLayoutComponentType::TrackQuickFilter,
            SharedLayoutComponentType::TrackPresentationButton,
            SharedLayoutComponentType::TrackCoverArt,
            SharedLayoutComponentType::PlaybackTransportButton,
            SharedLayoutComponentType::PlaybackSoulButton,
            SharedLayoutComponentType::PlaybackSeekSlider,
            SharedLayoutComponentType::PlaybackTimeLabel,
            SharedLayoutComponentType::PlaybackVolumeControl,
            SharedLayoutComponentType::PlaybackOutputDeviceSelector,
            SharedLayoutComponentType::StatusActivity,
            SharedLayoutComponentType::StatusTrackCount,
            SharedLayoutComponentType::StatusSelectionInfo,
            SharedLayoutComponentType::StatusMessage};
  }

  std::optional<SharedLayoutComponentType> sharedComponentFor(std::string_view const type) noexcept
  {
    for (auto const component : sharedLayoutComponentTypes())
    {
      if (componentTypeName(component) == type)
      {
        return component;
      }
    }

    return std::nullopt;
  }

  LayoutComponentDescriptor sharedComponentDescriptor(SharedLayoutComponentType const component)
  {
    switch (auto const type = componentTypeName(component); component)
    {
      case SharedLayoutComponentType::Box:
        return {.type = std::string{type},
                .displayName = "Box",
                .category = LayoutComponentCategory::Container,
                .props = {orientationProp(),
                          scalarProp(kSpacingProp, "Spacing", LayoutPropertyKind::Int, LayoutValue{std::int64_t{0}})}};

      case SharedLayoutComponentType::Split:
        // Only the axis is shared. Where an unauthored divider rests is a
        // measurement each toolkit takes differently, so the position property
        // stays with whichever shell can answer it.
        return {.type = std::string{type},
                .displayName = "Split Pane",
                .category = LayoutComponentCategory::Container,
                .props = {orientationProp()},
                .minChildren = 2,
                .optMaxChildren = 2,
                .persistentState = true};

      case SharedLayoutComponentType::Label:
        // What a shell does with the text before showing it is its own affair -
        // one resolves it against a resource dictionary first - but the authored
        // value is the words the reader sees either way.
        return leaf(type, "Label", LayoutComponentCategory::Generic, {stringProp(kTextProp, "Text")});

      case SharedLayoutComponentType::ActionButton:
      {
        auto descriptor =
          leaf(type, "Action Button", LayoutComponentCategory::Generic, {stringProp(kTextProp, "Text")});
        descriptor.actionPolicy = kExternalPrimaryActions;
        return descriptor;
      }

      case SharedLayoutComponentType::MenuButton:
        // Which menu the button presents is the shell's own inventory; what the
        // button says about itself is not.
        return leaf(type, "Menu Button", LayoutComponentCategory::Generic, {stringProp(kTextProp, "Text")});

      case SharedLayoutComponentType::MenuBar: return leaf(type, "Menu Bar", LayoutComponentCategory::Application);

      case SharedLayoutComponentType::TrackTable: return leaf(type, "Track Table", LayoutComponentCategory::Track);

      case SharedLayoutComponentType::TrackQuickFilter:
        return leaf(type, "Quick Filter", LayoutComponentCategory::Track);

      case SharedLayoutComponentType::TrackPresentationButton:
        // `default` is the button a shell draws when nothing asks for more; the
        // other two ask for a fuller or a tighter presentation, and a shell that
        // cannot draw one falls back rather than failing the document.
        return leaf(type,
                    "Presentation Button",
                    LayoutComponentCategory::Track,
                    {enumProp(kVariantProp, "Variant", {"default", "title", "compact"}, "default")});

      case SharedLayoutComponentType::TrackCoverArt:
        // How large the slot is belongs to the frame around it, so the only
        // shared question is what to draw when the selection carries no artwork.
        return leaf(
          type,
          "Cover Art",
          LayoutComponentCategory::Track,
          {enumProp(kPlaceholderStyleProp,
                    "Placeholder Style",
                    coverArtPlaceholderStyleIds(),
                    coverArtPlaceholderStyleId(defaultCoverArtPlaceholderStyle(CoverArtPlaceholderSlot::Inspector)))});

      case SharedLayoutComponentType::PlaybackTransportButton:
        // One button per command rather than one component type per command:
        // what changes between them is the transport verb, which is a property
        // of the button, not a different kind of button.
        return leaf(
          type,
          "Transport Button",
          LayoutComponentCategory::Playback,
          {enumProp(kCommandProp, "Command", playbackCommandIds(), playbackCommandId(PlaybackCommand::PlayPause))});

      case SharedLayoutComponentType::PlaybackSoulButton:
      {
        auto descriptor = leaf(
          type,
          "Soul Button",
          LayoutComponentCategory::Playback,
          // The inner mark itself is not shared: GTK's `glyph` picks which
          // of two ornaments the soul wears, while this shell draws the
          // live transport icon and only decides whether to draw it. One
          // name for two concepts is the divergence this vocabulary
          // exists to prevent, so each shell declares its own.
          {scalarProp(kStrokeWidthProp,
                      "Stroke Width",
                      LayoutPropertyKind::Double,
                      LayoutValue{kAobusSoulGeometry.baseStrokeWidth}),
           scalarProp(kGlyphScaleProp, "Glyph Scale", LayoutPropertyKind::Double, LayoutValue{kDefaultGlyphScale})});
        // The floor is what every shell can actually bind; GTK widens it.
        descriptor.actionPolicy = kExternalActionsWithoutSecondaryLongPress;
        return descriptor;
      }

      case SharedLayoutComponentType::PlaybackSeekSlider:
        return leaf(type, "Seek Slider", LayoutComponentCategory::Playback);

      case SharedLayoutComponentType::PlaybackTimeLabel:
        // The three readings are `PlaybackTimeMode`, which both shells format
        // through the same formatter.
        return leaf(type,
                    "Time Label",
                    LayoutComponentCategory::Playback,
                    {enumProp(kModeProp, "Mode", {"combined", "elapsed", "duration"}, "combined")});

      case SharedLayoutComponentType::PlaybackVolumeControl:
        return leaf(type, "Volume Control", LayoutComponentCategory::Playback);

      case SharedLayoutComponentType::PlaybackOutputDeviceSelector:
        return leaf(type, "Output Device Selector", LayoutComponentCategory::Playback);

      case SharedLayoutComponentType::StatusActivity:
        return leaf(type, "Activity Status", LayoutComponentCategory::Status);

      case SharedLayoutComponentType::StatusTrackCount:
        return leaf(type, "Track Count", LayoutComponentCategory::Status);

      case SharedLayoutComponentType::StatusSelectionInfo:
        return leaf(type, "Selection Info", LayoutComponentCategory::Status);

      case SharedLayoutComponentType::StatusMessage:
        return leaf(type, "Status Message", LayoutComponentCategory::Status);
    }

    return {};
  }

  namespace
  {
    using SlotDefault = std::pair<LayoutActionSlot, std::string>;
  } // namespace

  LayoutComponentDescriptor withShellActionSlots(LayoutComponentDescriptor descriptor,
                                                 LayoutComponentActionPolicy const& policy)
  {
    AO_EXPECTS((policy.slotMask & descriptor.actionPolicy.slotMask) == descriptor.actionPolicy.slotMask,
               "A shell may widen a shared action policy, never narrow it");
    descriptor.actionPolicy.slotMask = policy.slotMask;

    for (auto const& entry : policy.defaultActionIds)
    {
      auto const it = std::ranges::find(descriptor.actionPolicy.defaultActionIds, entry.first, &SlotDefault::first);

      if (it == descriptor.actionPolicy.defaultActionIds.end())
      {
        descriptor.actionPolicy.defaultActionIds.push_back(entry);
        continue;
      }

      it->second = entry.second;
    }

    return descriptor;
  }

  LayoutComponentDescriptor withShellProperties(LayoutComponentDescriptor descriptor,
                                                std::vector<LayoutPropertyDescriptor> properties)
  {
    for (auto& property : properties)
    {
      AO_EXPECTS(
        findProperty(descriptor.props, property.name) == nullptr, "A shell property may not restate a shared one");
      descriptor.props.push_back(std::move(property));
    }

    return descriptor;
  }

  LayoutComponentDescriptor withShellLayoutProperties(LayoutComponentDescriptor descriptor,
                                                      std::vector<LayoutPropertyDescriptor> properties)
  {
    for (auto& property : properties)
    {
      AO_EXPECTS(findProperty(descriptor.layoutProps, property.name) == nullptr,
                 "A shell layout property may not restate a shared one");
      descriptor.layoutProps.push_back(std::move(property));
    }

    return descriptor;
  }

  std::vector<std::string> sharedVocabularyDepartures(LayoutComponentCatalog const& catalog)
  {
    auto departures = std::vector<std::string>{};

    for (auto const& registered : catalog.descriptors())
    {
      auto const optComponent = sharedComponentFor(registered.type);

      if (!optComponent)
      {
        continue;
      }

      auto const shared = sharedComponentDescriptor(*optComponent);

      if (registered.displayName != shared.displayName)
      {
        departures.push_back(std::format(
          "{}: display name is '{}', shared is '{}'", registered.type, registered.displayName, shared.displayName));
      }

      if (registered.category != shared.category)
      {
        departures.push_back(std::format("{}: category is {}, shared is {}",
                                         registered.type,
                                         toString(registered.category),
                                         toString(shared.category)));
      }

      if (registered.minChildren != shared.minChildren || registered.optMaxChildren != shared.optMaxChildren)
      {
        departures.push_back(std::format("{}: child range is {}, shared is {}",
                                         registered.type,
                                         describeChildRange(registered),
                                         describeChildRange(shared)));
      }

      if (registered.persistentState != shared.persistentState)
      {
        departures.push_back(std::format("{}: persistent state is {}, shared is {}",
                                         registered.type,
                                         registered.persistentState,
                                         shared.persistentState));
      }

      // The shared mask is a floor, not an equality: every slot the vocabulary
      // names must be bindable everywhere, and a shell whose toolkit carries a
      // gesture the others lack may add to it through `withShellActionSlots`.
      // Which action fills a slot by default is drawn from the shell's own
      // inventory and cannot be the same string everywhere, so it is not
      // compared at all.
      if ((registered.actionPolicy.slotMask & shared.actionPolicy.slotMask) != shared.actionPolicy.slotMask)
      {
        departures.push_back(std::format("{}: action slots are {:#x}, which does not cover the shared {:#x}",
                                         registered.type,
                                         registered.actionPolicy.slotMask,
                                         shared.actionPolicy.slotMask));
      }

      for (auto const& sharedProp : shared.props)
      {
        auto const* const registeredProp = findProperty(registered.props, sharedProp.name);

        if (registeredProp == nullptr)
        {
          departures.push_back(std::format("{}: shared property '{}' is missing", registered.type, sharedProp.name));
          continue;
        }

        if (!describesSameProperty(sharedProp, *registeredProp))
        {
          departures.push_back(std::format(
            "{}: property '{}' is described differently from the shared one", registered.type, sharedProp.name));
        }
      }
    }

    return departures;
  }
} // namespace ao::uimodel

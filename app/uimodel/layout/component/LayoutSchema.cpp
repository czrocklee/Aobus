// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/layout/component/LayoutSchema.h>

#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    constexpr auto kAllActionSlots = std::array{ActionSlot::PrimaryClick,
                                                ActionSlot::PrimaryLongPress,
                                                ActionSlot::SecondaryClick,
                                                ActionSlot::SecondaryLongPress};
    constexpr auto kDefaultGlyphScale = 1.0;

    PropertySchema stringProperty(std::string_view const name, std::string_view const label)
    {
      return {.name = std::string{name},
              .kind = PropertyKind::String,
              .label = std::string{label},
              .defaultValue = LayoutValue{std::string{}}};
    }

    PropertySchema enumProperty(std::string_view const name,
                                std::string_view const label,
                                std::vector<std::string> values,
                                std::string_view const defaultValue)
    {
      return {.name = std::string{name},
              .kind = PropertyKind::Enum,
              .label = std::string{label},
              .defaultValue = LayoutValue{std::string{defaultValue}},
              .enumValues = std::move(values)};
    }

    PropertySchema scalarProperty(std::string_view const name,
                                  std::string_view const label,
                                  PropertyKind const kind,
                                  LayoutValue defaultValue)
    {
      return {
        .name = std::string{name}, .kind = kind, .label = std::string{label}, .defaultValue = std::move(defaultValue)};
    }

    PropertySchema orientationProperty()
    {
      return enumProperty(kOrientationProp, "Orientation", {"vertical", "horizontal"}, "vertical");
    }

    ComponentSchema leaf(std::string_view const id,
                         std::string_view const displayName,
                         ComponentCategory const category,
                         std::vector<PropertySchema> properties = {},
                         ActionSlotMask const actionSlots = 0)
    {
      return {.id = std::string{id},
              .displayName = std::string{displayName},
              .category = category,
              .properties = std::move(properties),
              .optMaxChildren = 0,
              .actionSlots = actionSlots};
    }

    PropertySchema const* findProperty(std::span<PropertySchema const> const properties, std::string_view const name)
    {
      auto const it = std::ranges::find(properties, name, &PropertySchema::name);
      return it == properties.end() ? nullptr : &*it;
    }

    bool appendProperties(std::vector<PropertySchema>& target, std::vector<PropertySchema> additions)
    {
      for (auto& property : additions)
      {
        if (findProperty(target, property.name) != nullptr)
        {
          return false;
        }

        target.push_back(std::move(property));
      }

      return true;
    }

    void mergeDefaultActions(ComponentSchema& schema, std::vector<DefaultActionBinding> additions)
    {
      for (auto& addition : additions)
      {
        auto const it = std::ranges::find(schema.defaultActions, addition.slot, &DefaultActionBinding::slot);

        if (it == schema.defaultActions.end())
        {
          schema.defaultActions.push_back(std::move(addition));
        }
        else
        {
          it->actionId = std::move(addition.actionId);
        }
      }
    }

    bool isActionProperty(std::string_view const name)
    {
      return std::ranges::any_of(
        kAllActionSlots, [name](ActionSlot const slot) { return LayoutSchema::actionProperty(slot) == name; });
    }

    void injectActionProperties(ComponentSchema& schema)
    {
      auto const inject = [&schema](std::string_view const name, std::string_view const label, ActionSlot const slot)
      {
        schema.properties.push_back({.name = std::string{name},
                                     .kind = PropertyKind::Enum,
                                     .label = std::string{label},
                                     .defaultValue = LayoutValue{std::string{}},
                                     .optActionSlot = slot});
      };

      if (schema.allows(ActionSlot::PrimaryClick))
      {
        inject(kPrimaryActionProp, "Primary Action", ActionSlot::PrimaryClick);
      }

      if (schema.allows(ActionSlot::PrimaryLongPress))
      {
        inject(kPrimaryLongPressActionProp, "Primary Long Press", ActionSlot::PrimaryLongPress);
      }

      if (schema.allows(ActionSlot::SecondaryClick))
      {
        inject(kSecondaryActionProp, "Secondary Action", ActionSlot::SecondaryClick);
      }

      if (schema.allows(ActionSlot::SecondaryLongPress))
      {
        inject(kSecondaryLongPressActionProp, "Secondary Long Press", ActionSlot::SecondaryLongPress);
      }
    }

    bool hasSharedPropertyShape(ComponentSchema const& candidate, ComponentSchema const& shared)
    {
      return std::ranges::all_of(
        shared.properties,
        [&candidate](PropertySchema const& sharedProperty)
        {
          auto const* const candidateProperty = findProperty(candidate.properties, sharedProperty.name);

          return candidateProperty != nullptr && candidateProperty->kind == sharedProperty.kind &&
                 candidateProperty->defaultValue.data == sharedProperty.defaultValue.data &&
                 candidateProperty->enumValues == sharedProperty.enumValues;
        });
    }

    bool preservesSharedVocabulary(ComponentSchema const& candidate)
    {
      auto const shared = sharedComponentSchemas();
      auto const it = std::ranges::find(shared, candidate.id, &ComponentSchema::id);

      if (it == shared.end())
      {
        return true;
      }

      return candidate.displayName == it->displayName && candidate.category == it->category &&
             candidate.minChildren == it->minChildren && candidate.optMaxChildren == it->optMaxChildren &&
             candidate.persistentState == it->persistentState &&
             (candidate.actionSlots & it->actionSlots) == it->actionSlots && hasSharedPropertyShape(candidate, *it);
    }
  } // namespace

  std::string_view ComponentSchema::defaultAction(ActionSlot const slot) const noexcept
  {
    auto const it = std::ranges::find(defaultActions, slot, &DefaultActionBinding::slot);
    return it == defaultActions.end() ? std::string_view{} : std::string_view{it->actionId};
  }

  std::optional<std::string_view> ComponentSchema::actionId(LayoutNode const& node, ActionSlot const slot) const
  {
    if (!allows(slot))
    {
      return std::nullopt;
    }

    if (auto const it = node.props.find(LayoutSchema::actionProperty(slot)); it != node.props.end())
    {
      auto const* const authoredId = it->second.getIf<std::string>();

      if (authoredId == nullptr || authoredId->empty() || *authoredId == "none")
      {
        return std::nullopt;
      }

      return *authoredId;
    }

    auto const fallback = defaultAction(slot);
    return fallback.empty() ? std::nullopt : std::optional{fallback};
  }

  ActionSlotMask ComponentSchema::boundActionSlots(LayoutNode const& node) const
  {
    ActionSlotMask result = 0;

    for (auto const slot : kAllActionSlots)
    {
      if (actionId(node, slot))
      {
        result |= actionSlotBit(slot);
      }
    }

    return result;
  }

  bool ComponentSchema::hasBoundAction(LayoutNode const& node) const
  {
    return boundActionSlots(node) != 0;
  }

  bool LayoutSchema::addComponent(ComponentSchema schema)
  {
    if (_componentIndexById.contains(schema.id) ||
        std::ranges::any_of(
          schema.properties, [](PropertySchema const& property) { return isActionProperty(property.name); }) ||
        std::ranges::any_of(schema.defaultActions,
                            [&schema](DefaultActionBinding const& binding) { return !schema.allows(binding.slot); }) ||
        !preservesSharedVocabulary(schema))
    {
      return false;
    }

    injectActionProperties(schema);
    _componentIndexById[schema.id] = _components.size();
    _components.push_back(std::move(schema));
    return true;
  }

  bool LayoutSchema::addSharedComponent(std::string_view const id, ComponentSchemaExtension extension)
  {
    auto const shared = sharedComponentSchemas();
    auto const it = std::ranges::find(shared, id, &ComponentSchema::id);

    if (it == shared.end())
    {
      return false;
    }

    auto schema = *it;

    if (!appendProperties(schema.properties, std::move(extension.properties)) ||
        !appendProperties(schema.layoutProperties, std::move(extension.layoutProperties)))
    {
      return false;
    }

    schema.actionSlots |= extension.actionSlots;
    mergeDefaultActions(schema, std::move(extension.defaultActions));

    if (std::ranges::any_of(schema.defaultActions,
                            [&schema](DefaultActionBinding const& binding) { return !schema.allows(binding.slot); }))
    {
      return false;
    }

    return addComponent(std::move(schema));
  }

  bool LayoutSchema::addAction(ActionSchema schema)
  {
    if (_actionIndexById.contains(schema.id))
    {
      return false;
    }

    _actionIndexById[schema.id] = _actions.size();
    _actions.push_back(std::move(schema));
    return true;
  }

  std::optional<ComponentSchema> LayoutSchema::component(std::string_view const id) const
  {
    if (auto const it = _componentIndexById.find(id); it != _componentIndexById.end())
    {
      return _components[it->second];
    }

    return std::nullopt;
  }

  std::optional<ActionSchema> LayoutSchema::action(std::string_view const id) const
  {
    if (auto const it = _actionIndexById.find(id); it != _actionIndexById.end())
    {
      return _actions[it->second];
    }

    return std::nullopt;
  }

  std::span<ComponentSchema const> sharedComponentSchemas()
  {
    static auto const schemas = std::vector<ComponentSchema>{
      {.id = "box",
       .displayName = "Box",
       .category = ComponentCategory::Container,
       .properties = {orientationProperty(),
                      scalarProperty(kSpacingProp, "Spacing", PropertyKind::Int, LayoutValue{std::int64_t{0}})}},
      {.id = "split",
       .displayName = "Split Pane",
       .category = ComponentCategory::Container,
       .properties = {orientationProperty()},
       .minChildren = 2,
       .optMaxChildren = 2,
       .persistentState = true},
      leaf("label", "Label", ComponentCategory::Generic, {stringProperty(kTextProp, "Text")}),
      leaf("actionButton",
           "Action Button",
           ComponentCategory::Generic,
           {stringProperty(kTextProp, "Text")},
           actionSlotBit(ActionSlot::PrimaryClick) | actionSlotBit(ActionSlot::PrimaryLongPress)),
      leaf("menuButton", "Menu Button", ComponentCategory::Generic, {stringProperty(kTextProp, "Text")}),
      leaf("app.menuBar", "Menu Bar", ComponentCategory::Application),
      leaf("track.table", "Track Table", ComponentCategory::Track),
      leaf("track.quickFilter", "Quick Filter", ComponentCategory::Track),
      leaf("track.presentationButton",
           "Presentation Button",
           ComponentCategory::Track,
           {enumProperty(kVariantProp, "Variant", {"default", "title", "compact"}, "default")}),
      leaf("track.coverArt",
           "Cover Art",
           ComponentCategory::Track,
           {enumProperty(
             kPlaceholderStyleProp,
             "Placeholder Style",
             coverArtPlaceholderStyleIds(),
             coverArtPlaceholderStyleId(defaultCoverArtPlaceholderStyle(CoverArtPlaceholderSlot::Inspector)))}),
      leaf(
        "playback.transportButton",
        "Transport Button",
        ComponentCategory::Playback,
        {enumProperty(kCommandProp, "Command", playbackCommandIds(), playbackCommandId(PlaybackCommand::PlayPause))}),
      leaf("playback.soulButton",
           "Soul Button",
           ComponentCategory::Playback,
           {scalarProperty(
              kStrokeWidthProp, "Stroke Width", PropertyKind::Double, LayoutValue{kAobusSoulGeometry.baseStrokeWidth}),
            scalarProperty(kGlyphScaleProp, "Glyph Scale", PropertyKind::Double, LayoutValue{kDefaultGlyphScale})},
           actionSlotBit(ActionSlot::PrimaryClick) | actionSlotBit(ActionSlot::PrimaryLongPress) |
             actionSlotBit(ActionSlot::SecondaryClick)),
      leaf("playback.seekSlider", "Seek Slider", ComponentCategory::Playback),
      leaf("playback.timeLabel",
           "Time Label",
           ComponentCategory::Playback,
           {enumProperty(kModeProp, "Mode", {"combined", "elapsed", "duration"}, "combined")}),
      leaf("playback.volumeControl", "Volume Control", ComponentCategory::Playback),
      leaf("playback.outputDeviceSelector", "Output Device Selector", ComponentCategory::Playback),
      leaf("status.activity", "Activity Status", ComponentCategory::Status),
      leaf("status.trackCount", "Track Count", ComponentCategory::Status),
      leaf("status.selectionInfo", "Selection Info", ComponentCategory::Status),
      leaf("status.message", "Status Message", ComponentCategory::Status),
    };

    return schemas;
  }
} // namespace ao::uimodel

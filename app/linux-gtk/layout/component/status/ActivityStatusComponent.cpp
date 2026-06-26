// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "status/ActivityStatus.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/library/Library.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/widget.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    ActivityStatusVariant parseVariant(std::string const& value)
    {
      if (value == "classicInline")
      {
        return ActivityStatusVariant::ClassicInline;
      }

      return ActivityStatusVariant::Ambient;
    }

    ActivityStatusIdleBehavior parseIdleBehavior(std::string const& value, ActivityStatusVariant const variant)
    {
      if (value == "reserve")
      {
        return ActivityStatusIdleBehavior::Reserve;
      }

      if (value == "hidden")
      {
        return ActivityStatusIdleBehavior::Hidden;
      }

      return variant == ActivityStatusVariant::ClassicInline ? ActivityStatusIdleBehavior::Reserve
                                                             : ActivityStatusIdleBehavior::Hidden;
    }

    constexpr std::int32_t kMaxTextCharsMin = 8;
    constexpr std::int32_t kMaxTextCharsMax = 120;

    ActivityStatusOptions optionsFromNode(LayoutNode const& node)
    {
      auto const variant = parseVariant(node.getProp<std::string>("variant", "ambient"));
      auto const rawMaxTextChars = node.getProp<std::int64_t>("maxTextChars", kDefaultMaxTextChars);
      return ActivityStatusOptions{
        .variant = variant,
        .idleBehavior = parseIdleBehavior(node.getProp<std::string>("idleBehavior", ""), variant),
        .maxTextChars = static_cast<std::int32_t>(
          std::clamp(rawMaxTextChars, std::int64_t{kMaxTextCharsMin}, std::int64_t{kMaxTextCharsMax})),
      };
    }

    std::string componentIdFromNode(LayoutNode const& node)
    {
      return node.id.empty() ? node.type : node.id;
    }

    class ActivityStatusComponent final : public ILayoutComponent
    {
    public:
      ActivityStatusComponent(LayoutContext& ctx, LayoutNode const& node)
        : _runtime{ctx.runtime}
        , _parentWindow{ctx.parentWindow}
        , _actionRegistry{ctx.actionRegistry}
        , _componentId{componentIdFromNode(node)}
        , _widget{ActivityStatusDependencies{
            .notifications = ctx.runtime.notifications(),
            .libraryChanges = &ctx.runtime.library().changes(),
            .options = optionsFromNode(node),
            .resolveNotificationAction =
              [this](std::string_view actionId, std::string_view actionLabel)
            {
              auto const optDesc = _actionRegistry.descriptor(actionId);

              if (!optDesc)
              {
                return ActivityStatusActionRenderState{};
              }

              auto activationContext = ActionActivationContext{.runtime = _runtime,
                                                               .parentWindow = _parentWindow,
                                                               .anchorWidget = _widget.widget(),
                                                               .componentId = _componentId};
              auto const actionState = _actionRegistry.state(actionId, activationContext);
              auto label = actionLabel.empty() ? optDesc->label : std::string{actionLabel};

              return ActivityStatusActionRenderState{.visible = !label.empty(),
                                                     .enabled = actionState.enabled,
                                                     .label = std::move(label),
                                                     .disabledReason = actionState.disabledReason};
            },
            .onNotificationAction =
              [this](auto, auto actionId, Gtk::Widget& anchor)
            {
              auto activationContext = ActionActivationContext{.runtime = _runtime,
                                                               .parentWindow = _parentWindow,
                                                               .anchorWidget = anchor,
                                                               .componentId = _componentId};
              _actionRegistry.tryActivate(actionId, activationContext);
            }}}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      rt::AppRuntime& _runtime;
      Gtk::Window& _parentWindow;
      ActionRegistry const& _actionRegistry;
      std::string _componentId;
      ActivityStatus _widget;
    };

    std::unique_ptr<ILayoutComponent> createActivityStatus(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<ActivityStatusComponent>(ctx, node);
    }
  } // namespace

  void registerActivityStatusComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "status.activityStatus",
                                .displayName = "Activity Status",
                                .category = ComponentCategory::Status,
                                .props = {{.name = "variant",
                                           .kind = PropertyKind::Enum,
                                           .label = "Variant",
                                           .defaultValue = LayoutValue{"ambient"},
                                           .enumValues = {"ambient", "classicInline"}},
                                          {.name = "idleBehavior",
                                           .kind = PropertyKind::Enum,
                                           .label = "Idle Behavior",
                                           .defaultValue = LayoutValue{""},
                                           .enumValues = {"", "hidden", "reserve"}},
                                          {.name = "maxTextChars",
                                           .kind = PropertyKind::Int,
                                           .label = "Max Text Chars",
                                           .defaultValue = LayoutValue{kDefaultMaxTextChars}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createActivityStatus);
  }
} // namespace ao::gtk::layout

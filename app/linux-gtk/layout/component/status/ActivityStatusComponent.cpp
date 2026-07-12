// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "status/ActivityStatusWidget.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/library/Library.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    ActivityStatusWidgetVariant parseVariant(std::string const& value)
    {
      if (value == "classicInline")
      {
        return ActivityStatusWidgetVariant::ClassicInline;
      }

      return ActivityStatusWidgetVariant::Ambient;
    }

    ActivityStatusWidgetIdleBehavior parseIdleBehavior(std::string const& value,
                                                       ActivityStatusWidgetVariant const variant)
    {
      if (value == "reserve")
      {
        return ActivityStatusWidgetIdleBehavior::Reserve;
      }

      if (value == "hidden")
      {
        return ActivityStatusWidgetIdleBehavior::Hidden;
      }

      return variant == ActivityStatusWidgetVariant::ClassicInline ? ActivityStatusWidgetIdleBehavior::Reserve
                                                                   : ActivityStatusWidgetIdleBehavior::Hidden;
    }

    constexpr std::int32_t kMaxTextCharsMin = 8;
    constexpr std::int32_t kMaxTextCharsMax = 120;

    ActivityStatusWidgetOptions optionsFromNode(LayoutNode const& node)
    {
      auto const variant = parseVariant(node.propertyOr<std::string>("variant", "ambient"));
      auto const rawMaxTextChars = node.propertyOr<std::int64_t>("maxTextChars", kDefaultMaxTextChars);
      return ActivityStatusWidgetOptions{
        .variant = variant,
        .idleBehavior = parseIdleBehavior(node.propertyOr<std::string>("idleBehavior", ""), variant),
        .maxTextChars = static_cast<std::int32_t>(
          std::clamp(rawMaxTextChars, std::int64_t{kMaxTextCharsMin}, std::int64_t{kMaxTextCharsMax})),
      };
    }

    std::string componentIdFromNode(LayoutNode const& node)
    {
      return node.id.empty() ? node.type : node.id;
    }

    class ActivityStatusComponent final : public LayoutComponent
    {
    public:
      ActivityStatusComponent(LayoutBuildContext& ctx, LayoutNode const& node)
        : _runtime{ctx.runtime}
        , _parentWindow{ctx.parentWindow}
        , _actionRegistry{ctx.actionRegistry}
        , _componentId{componentIdFromNode(node)}
        , _widget{ActivityStatusWidgetDependencies{
            .notifications = ctx.runtime.notifications(),
            .libraryChanges = &ctx.runtime.library().changes(),
            .options = optionsFromNode(node),
            .resolveNotificationAction =
              [this](std::string_view actionId, std::string_view actionLabel)
            {
              auto const optDesc = _actionRegistry.descriptor(actionId);

              if (!optDesc)
              {
                return ActivityStatusWidgetActionRenderState{};
              }

              auto activationContext = ActionActivationContext{.runtime = _runtime,
                                                               .parentWindow = _parentWindow,
                                                               .anchorWidget = _widget.widget(),
                                                               .componentId = _componentId};
              auto const actionState = _actionRegistry.state(actionId, activationContext);
              auto label = actionLabel.empty() ? optDesc->label : std::string{actionLabel};

              return ActivityStatusWidgetActionRenderState{.visible = !label.empty(),
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
      ActivityStatusWidget _widget;
    };

    std::unique_ptr<LayoutComponent> createActivityStatus(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<ActivityStatusComponent>(ctx, node);
    }
  } // namespace

  void registerActivityStatusComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "status.activityStatus",
                                .displayName = "Activity Status",
                                .category = LayoutComponentCategory::Status,
                                .props = {{.name = "variant",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Variant",
                                           .defaultValue = LayoutValue{"ambient"},
                                           .enumValues = {"ambient", "classicInline"}},
                                          {.name = "idleBehavior",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Idle Behavior",
                                           .defaultValue = LayoutValue{""},
                                           .enumValues = {"", "hidden", "reserve"}},
                                          {.name = "maxTextChars",
                                           .kind = LayoutPropertyKind::Int,
                                           .label = "Max Text Chars",
                                           .defaultValue = LayoutValue{kDefaultMaxTextChars}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createActivityStatus);
  }
} // namespace ao::gtk::layout

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::rt
{
  class AppRuntime;
}
namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::gtk::layout
{
  class ComponentRegistry;

  /**
   * @brief Register status-related layout components.
   */
  void registerStatusComponents(ComponentRegistry& registry,
                                rt::AppRuntime& runtime,
                                i18n::MessageCatalog const& textCatalog);
} // namespace ao::gtk::layout

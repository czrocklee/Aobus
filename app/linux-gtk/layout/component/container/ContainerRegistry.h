// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::gtk::layout
{
  class ComponentRegistry;

  /**
   * @brief Register the built-in container components.
   */
  void registerContainerComponents(ComponentRegistry& registry, i18n::MessageCatalog const& textCatalog);
} // namespace ao::gtk::layout

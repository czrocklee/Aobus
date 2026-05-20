// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutController.h"

#include "ao/utility/Log.h"
#include "layout/document/LayoutDocument.h"
#include "layout/editor/LayoutEditorDialog.h"
#include "layout/runtime/LayoutRuntime.h"
#include "runtime/AppRuntime.h"
#include "runtime/ConfigStore.h"
#include "runtime/async/LifetimeScope.h"
#include "runtime/async/Runtime.h"
#include "runtime/async/Task.h"

#include <gtkmm/dialog.h>
#include <gtkmm/window.h>

#include <memory>
#include <string>

namespace ao::gtk
{
  ShellLayoutController::ShellLayoutController(rt::AppRuntime& runtime, Gtk::Window& parentWindow)
    : _registry{}, _context{.registry = _registry, .runtime = runtime, .parentWindow = parentWindow}, _host{_registry}
  {
    layout::LayoutRuntime::registerStandardComponents(_registry);
  }

  void ShellLayoutController::attachToWindow()
  {
    _context.parentWindow.set_child(_host);
  }

  void ShellLayoutController::loadLayout(rt::ConfigStore& configStore)
  {
    auto& runtime = _context.runtime.async();
    runtime.spawnWithLifetime(&_tasks,
                              [](ShellLayoutController* self, rt::ConfigStore* store) -> rt::async::Task<void>
                              {
                                APP_LOG_DEBUG("ShellLayoutController: loadLayout coroutine started on UI thread");

                                auto* const asyncRuntime = &self->_context.runtime.async();
                                co_await asyncRuntime->resumeOnWorker();
                                APP_LOG_DEBUG("ShellLayoutController: loading config on background worker thread");

                                auto doc = layout::createDefaultLayout();

                                if (auto const res = layout::loadLayout(*store, "linuxGtkLayout", doc);
                                    !res && res.error().code != Error::Code::NotFound)
                                {
                                  APP_LOG_DEBUG("Failed to load layout from config: {}", res.error().message);
                                }

                                co_await asyncRuntime->resumeOnControl();
                                APP_LOG_DEBUG("ShellLayoutController: resumed on UI thread, applying layout");

                                self->_activeLayout = doc;
                                self->_host.setLayout(self->_context, self->_activeLayout);
                              }(this, &configStore));
  }

  void ShellLayoutController::saveLayout(rt::ConfigStore& configStore) const
  {
    layout::saveLayout(configStore, "linuxGtkLayout", _activeLayout);
  }

  void ShellLayoutController::openEditor(rt::ConfigStore& configStore)
  {
    auto const dialog = std::make_shared<layout::editor::LayoutEditorDialog>(
      dynamic_cast<Gtk::Window&>(_context.parentWindow), _registry, _activeLayout);
    auto* const dialogPtr = dialog.get();

    _context.editMode = true;
    _context.onNodeMoved = [dialogPtr](std::string const& nodeId, int posX, int posY)
    { dialogPtr->updateNodePosition(nodeId, posX, posY); };

    _host.setLayout(_context, _activeLayout);

    dialogPtr->signalApplyPreview().connect([this](layout::LayoutDocument const& doc)
                                            { _host.setLayout(_context, doc); });

    dialogPtr->signal_response().connect(
      [this, sharedDialog = dialog, &configStore](int responseId)
      {
        _context.editMode = false;
        _context.onNodeMoved = nullptr;

        if (responseId == Gtk::ResponseType::OK)
        {
          _activeLayout = sharedDialog->document();
          _host.setLayout(_context, _activeLayout);
          layout::saveLayout(configStore, "linuxGtkLayout", _activeLayout);
        }
        else if (responseId == Gtk::ResponseType::CANCEL)
        {
          _host.setLayout(_context, _activeLayout);
        }

        sharedDialog->close();
      });

    dialogPtr->present();
  }
} // namespace ao::gtk

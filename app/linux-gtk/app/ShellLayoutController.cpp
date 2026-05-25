// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellLayoutController.h"

#include "AppConfig.h"
#include "ao/rt/async/Runtime.h"
#include "ao/rt/async/Task.h"
#include "layout/document/LayoutDocument.h"
#include "layout/editor/LayoutEditorDialog.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutContext.h"
#include "layout/runtime/LayoutHost.h"
#include "layout/runtime/LayoutRuntime.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/StateTypes.h>
#include <ao/utility/Log.h>

#include <gtkmm/dialog.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

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

  void ShellLayoutController::loadLayout(AppConfig& config)
  {
    auto& runtime = _context.runtime.async();
    runtime.spawnWithLifetime(
      &_tasks,
      [](ShellLayoutController* self, AppConfig* cfg) -> rt::async::Task<void>
      {
        APP_LOG_DEBUG("ShellLayoutController: loadLayout coroutine started on UI thread");

        auto* const asyncRuntime = &self->_context.runtime.async();
        co_await asyncRuntime->resumeOnWorker();
        APP_LOG_DEBUG("ShellLayoutController: loading config on background worker thread");

        auto prefs = rt::AppPrefsState{};
        cfg->loadAppPrefs(prefs);

        auto presetId = layout::LayoutPresetId::Classic;
        auto presetIdStr = std::string{"classic"};

        if (prefs.lastLayoutPreset == "modern")
        {
          presetId = layout::LayoutPresetId::Modern;
          presetIdStr = "modern";
        }
        else if (!prefs.lastLayoutPreset.empty() && prefs.lastLayoutPreset != "classic")
        {
          APP_LOG_DEBUG(
            "ShellLayoutController: Unknown layout preset '{}', falling back to classic", prefs.lastLayoutPreset);
        }

        auto doc = layout::createBuiltInLayout(presetId);
        cfg->loadShellLayout(doc, presetIdStr);

        co_await asyncRuntime->resumeOnControl();
        APP_LOG_DEBUG("ShellLayoutController: resumed on UI thread, applying layout");

        self->_activePresetId = std::move(presetIdStr);
        self->_activeLayout = doc;
        self->_host.setLayout(self->_context, self->_activeLayout);
      }(this, &config));
  }

  void ShellLayoutController::saveLayout(AppConfig& config) const
  {
    config.saveShellLayout(_activeLayout, _activePresetId);
  }

  void ShellLayoutController::openEditor(AppConfig& config)
  {
    auto prefs = rt::AppPrefsState{};
    config.loadAppPrefs(prefs);

    auto const initialPresetId = _activePresetId.empty() ? "classic" : _activePresetId;

    auto const dialog = std::make_shared<layout::editor::LayoutEditorDialog>(
      dynamic_cast<Gtk::Window&>(_context.parentWindow), _registry, _activeLayout, initialPresetId);
    auto* const dialogPtr = dialog.get();

    _context.editMode = true;
    _context.onNodeMoved = [dialogPtr](std::string const& nodeId, std::int32_t posX, std::int32_t posY)
    { dialogPtr->updateNodePosition(nodeId, posX, posY); };

    _host.setLayout(_context, _activeLayout);

    dialogPtr->signalApplyPreview().connect([this](layout::LayoutDocument const& doc)
                                            { _host.setLayout(_context, doc); });

    dialogPtr->signal_response().connect(
      [this, sharedDialog = dialog, &config](std::int32_t responseId)
      {
        _context.editMode = false;
        _context.onNodeMoved = nullptr;

        if (responseId == Gtk::ResponseType::OK)
        {
          _activeLayout = sharedDialog->document();

          if (auto const presetIdDialog = sharedDialog->getSelectedPresetId(); !presetIdDialog.empty())
          {
            _activePresetId = presetIdDialog;

            auto prefsUpdate = rt::AppPrefsState{};
            config.loadAppPrefs(prefsUpdate);
            prefsUpdate.lastLayoutPreset = _activePresetId;
            config.saveAppPrefs(prefsUpdate);
          }

          _host.setLayout(_context, _activeLayout);
          config.saveShellLayout(_activeLayout, _activePresetId);
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

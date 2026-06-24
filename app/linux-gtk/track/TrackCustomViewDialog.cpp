// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackCustomViewDialog.h"

#include "app/AppDialog.h"
#include "app/FormBuilder.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/track/TrackCustomViewEditorModel.h>

#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace ao::gtk
{
  namespace
  {
    constexpr int kBoxSpacing = 6;
    constexpr int kMinScrollContentWidth = 480;
    constexpr int kMaxScrollContentWidth = 700;
    constexpr std::int32_t kMaxScrollContentHeight = 520;

    std::string generateId()
    {
      static std::random_device rd;
      static std::mt19937_64 gen{rd()};
      static std::uniform_int_distribution<std::uint64_t> dis;
      return std::format("{:016x}", dis(gen));
    }

    Glib::RefPtr<Gtk::StringList> createGroupKeysModel(std::span<uimodel::track::TrackGroupKeyOption const> options)
    {
      auto modelPtr = Gtk::StringList::create({});

      for (auto const& option : options)
      {
        modelPtr->append(option.label);
      }

      return modelPtr;
    }

    Glib::RefPtr<Gtk::StringList> createSortFieldsModel(std::span<uimodel::track::TrackSortFieldOption const> options)
    {
      auto modelPtr = Gtk::StringList::create({});

      for (auto const& option : options)
      {
        modelPtr->append(option.label);
      }

      return modelPtr;
    }

    Glib::RefPtr<Gtk::StringList> createVisibleFieldsModel(
      std::span<uimodel::track::TrackVisibleFieldOption const> options)
    {
      auto modelPtr = Gtk::StringList::create({});

      for (auto const& option : options)
      {
        modelPtr->append(option.label);
      }

      return modelPtr;
    }
  } // namespace

  TrackCustomViewDialog::TrackCustomViewDialog(Gtk::Window& parent,
                                               rt::TrackPresentationSpec const& initialSpec,
                                               std::string_view initialLabel)
    : AppDialog{}
  {
    set_title("Edit Custom View");
    set_transient_for(parent);

    set_default_size(-1, -1);

    setupUi();
    populateFromSpec(initialSpec, initialLabel);
  }

  void TrackCustomViewDialog::setupUi()
  {
    addCancelAction("Cancel", Gtk::ResponseType::CANCEL);
    addPrimaryAction("Save", Gtk::ResponseType::OK);

    auto* mainBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, kBoxSpacing * 2);

    // Section 1: Metadata Card
    auto* metaList = Gtk::make_managed<FormBoxedList>();
    _nameEntry.set_placeholder_text("View label");
    metaList->addEntryRow("Name", _nameEntry);

    _groupDropdown.set_model(createGroupKeysModel(_model.groupOptions()));
    _groupDropdown.property_selected().signal_changed().connect(
      [this]
      {
        if (auto const selected = _groupDropdown.get_selected(); selected < _model.groupOptions().size())
        {
          _model.setGroupKeyByOptionIndex(selected);
        }
      });
    metaList->addRow("Group By", _groupDropdown);
    mainBox->append(*metaList);

    // Sort Terms
    auto* sortLabel = Gtk::make_managed<Gtk::Label>("Sort Order");
    sortLabel->set_halign(Gtk::Align::START);
    sortLabel->add_css_class("ao-section-header");
    mainBox->append(*sortLabel);

    _sortTermsList.add_css_class("ao-boxed-list");
    _sortTermsList.set_selection_mode(Gtk::SelectionMode::NONE);
    mainBox->append(_sortTermsList);

    auto* addSortBtn = Gtk::make_managed<Gtk::Button>("Add Sort Field");
    addSortBtn->set_halign(Gtk::Align::START);
    addSortBtn->signal_clicked().connect(
      [this]
      {
        _model.addSortTerm();
        rebuildSortList();
      });
    mainBox->append(*addSortBtn);

    // Visible Fields
    auto* fieldsLabel = Gtk::make_managed<Gtk::Label>("Visible Columns");
    fieldsLabel->set_halign(Gtk::Align::START);
    fieldsLabel->add_css_class("ao-section-header");
    mainBox->append(*fieldsLabel);

    _visibleFieldsList.add_css_class("ao-boxed-list");
    _visibleFieldsList.set_selection_mode(Gtk::SelectionMode::NONE);
    mainBox->append(_visibleFieldsList);

    auto* addVisibleBtn = Gtk::make_managed<Gtk::Button>("Add Column");
    addVisibleBtn->set_halign(Gtk::Align::START);
    addVisibleBtn->signal_clicked().connect(
      [this]
      {
        _model.addVisibleField();
        rebuildVisibleFieldsList();
      });
    mainBox->append(*addVisibleBtn);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_propagate_natural_width(true);
    scroll->set_propagate_natural_height(true);
    scroll->set_min_content_width(kMinScrollContentWidth);
    scroll->set_max_content_width(kMaxScrollContentWidth);
    scroll->set_max_content_height(kMaxScrollContentHeight);
    scroll->set_child(*mainBox);
    scroll->set_vexpand(true);

    setContentWidget(*scroll);
  }

  void TrackCustomViewDialog::rebuildSortList()
  {
    while (auto* child = _sortTermsList.get_first_child())
    {
      _sortTermsList.remove(*child);
    }

    auto const sortTerms = _model.sortTerms();

    for (std::size_t i = 0; i < sortTerms.size(); ++i)
    {
      auto const& term = sortTerms[i];
      auto* const row = Gtk::make_managed<Gtk::ListBoxRow>();
      auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kBoxSpacing);

      auto* const dropdown = Gtk::make_managed<Gtk::DropDown>(createSortFieldsModel(_model.sortFieldOptions()));
      auto const index = _model.optionIndexForSortField(term.field).value_or(0);

      dropdown->set_selected(static_cast<::guint>(index));
      dropdown->property_selected().signal_changed().connect(
        [this, i, dropdown]
        {
          if (auto const selected = dropdown->get_selected(); selected < _model.sortFieldOptions().size())
          {
            _model.setSortFieldByOptionIndex(i, selected);
          }
        });
      box->append(*dropdown);

      auto* ascBtn = Gtk::make_managed<Gtk::ToggleButton>("Ascending");
      ascBtn->set_active(term.ascending);

      ascBtn->signal_toggled().connect([this, i, ascBtn] { _model.setSortAscending(i, ascBtn->get_active()); });
      box->append(*ascBtn);

      auto* spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      spacer->set_hexpand(true);
      box->append(*spacer);

      auto* upBtn = Gtk::make_managed<Gtk::Button>("Up");
      upBtn->set_sensitive(i > 0);

      upBtn->signal_clicked().connect(
        [this, i]
        {
          _model.moveSortTermUp(i);
          rebuildSortList();
        });
      box->append(*upBtn);

      auto* downBtn = Gtk::make_managed<Gtk::Button>("Down");
      downBtn->set_sensitive(i + 1 < sortTerms.size());

      downBtn->signal_clicked().connect(
        [this, i]
        {
          _model.moveSortTermDown(i);
          rebuildSortList();
        });
      box->append(*downBtn);

      auto* removeBtn = Gtk::make_managed<Gtk::Button>("Remove");
      removeBtn->signal_clicked().connect(
        [this, i]
        {
          _model.removeSortTerm(i);
          rebuildSortList();
        });
      box->append(*removeBtn);

      row->set_child(*box);
      _sortTermsList.append(*row);
    }
  }

  void TrackCustomViewDialog::rebuildVisibleFieldsList()
  {
    while (auto* child = _visibleFieldsList.get_first_child())
    {
      _visibleFieldsList.remove(*child);
    }

    auto const visibleFields = _model.visibleFields();

    for (std::size_t i = 0; i < visibleFields.size(); ++i)
    {
      auto const field = visibleFields[i];
      auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
      auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kBoxSpacing);

      auto* dropdown = Gtk::make_managed<Gtk::DropDown>(createVisibleFieldsModel(_model.visibleFieldOptions()));
      auto const index = _model.optionIndexForVisibleField(field).value_or(0);

      dropdown->set_selected(static_cast<::guint>(index));
      dropdown->property_selected().signal_changed().connect(
        [this, i, dropdown]
        {
          if (auto const selected = dropdown->get_selected(); selected < _model.visibleFieldOptions().size())
          {
            _model.setVisibleFieldByOptionIndex(i, selected);
          }
        });
      box->append(*dropdown);

      auto* spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      spacer->set_hexpand(true);
      box->append(*spacer);

      auto* upBtn = Gtk::make_managed<Gtk::Button>("Up");
      upBtn->set_sensitive(i > 0);

      upBtn->signal_clicked().connect(
        [this, i]
        {
          _model.moveVisibleFieldUp(i);
          rebuildVisibleFieldsList();
        });
      box->append(*upBtn);

      auto* downBtn = Gtk::make_managed<Gtk::Button>("Down");
      downBtn->set_sensitive(i + 1 < visibleFields.size());

      downBtn->signal_clicked().connect(
        [this, i]
        {
          _model.moveVisibleFieldDown(i);
          rebuildVisibleFieldsList();
        });
      box->append(*downBtn);

      auto* removeBtn = Gtk::make_managed<Gtk::Button>("Remove");
      removeBtn->signal_clicked().connect(
        [this, i]
        {
          _model.removeVisibleField(i);
          rebuildVisibleFieldsList();
        });
      box->append(*removeBtn);

      row->set_child(*box);
      _visibleFieldsList.append(*row);
    }
  }

  void TrackCustomViewDialog::populateFromSpec(rt::TrackPresentationSpec const& spec, std::string_view label)
  {
    _model.populate(spec, label);
    _nameEntry.set_text(std::string{_model.label()});

    if (auto const optIndex = _model.groupKeyOptionIndex(); optIndex)
    {
      _groupDropdown.set_selected(static_cast<::guint>(*optIndex));
    }

    rebuildSortList();
    rebuildVisibleFieldsList();
  }

  rt::CustomTrackPresentationPreset TrackCustomViewDialog::collectState()
  {
    _model.setLabel(_nameEntry.get_text().raw());
    return _model.collectState(generateId());
  }

  std::optional<TrackCustomViewDialog::Result> TrackCustomViewDialog::runDialog()
  {
    show();

    auto loopPtr = Glib::MainLoop::create(false);
    auto response = Gtk::ResponseType::CANCEL;

    signal_response().connect(
      [&loopPtr, &response](std::int32_t resp)
      {
        response = static_cast<Gtk::ResponseType>(resp);
        loopPtr->quit();
      });

    loopPtr->run();
    hide();

    if (response == Gtk::ResponseType::OK)
    {
      return Result{.state = collectState(), .deleted = false};
    }

    return std::nullopt;
  }
} // namespace ao::gtk

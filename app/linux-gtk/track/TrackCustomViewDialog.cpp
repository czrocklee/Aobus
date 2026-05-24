// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackCustomViewDialog.h"

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <glibmm/main.h>
#include <glibmm/refptr.h>
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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr int kDefaultWidth = 500;
    constexpr int kDefaultHeight = 600;
    constexpr int kBoxSpacing = 6;

    std::string generateId()
    {
      static std::random_device rd;
      static std::mt19937_64 gen{rd()};
      static std::uniform_int_distribution<std::uint64_t> dis;
      return std::format("{:016x}", dis(gen));
    }

    std::string_view groupKeyName(rt::TrackGroupKey key)
    {
      switch (key)
      {
        case rt::TrackGroupKey::None: return "None";
        case rt::TrackGroupKey::Artist: return "Artist";
        case rt::TrackGroupKey::Album: return "Album";
        case rt::TrackGroupKey::AlbumArtist: return "Album Artist";
        case rt::TrackGroupKey::Genre: return "Genre";
        case rt::TrackGroupKey::Composer: return "Composer";
        case rt::TrackGroupKey::Work: return "Work";
        case rt::TrackGroupKey::Year: return "Year";
      }

      return "None";
    }

    Glib::RefPtr<Gtk::StringList> createSortFieldsModel(std::vector<rt::TrackSortField>& mapping)
    {
      auto model = Gtk::StringList::create({});
      auto const defs = rt::trackFieldDefinitions();
      mapping.clear();

      for (std::size_t idx = 0; idx < rt::kTrackSortFieldCount; ++idx)
      {
        auto const sortField = static_cast<rt::TrackSortField>(idx);

        for (auto const& def : defs)
        {
          if (def.optSortField == sortField)
          {
            model->append(std::string{def.label});
            mapping.push_back(sortField);
            break;
          }
        }
      }

      return model;
    }

    Glib::RefPtr<Gtk::StringList> createVisibleFieldsModel(std::vector<rt::TrackField>& mapping)
    {
      auto model = Gtk::StringList::create({});
      mapping.clear();

      for (auto const& def : rt::trackFieldDefinitions())
      {
        if (def.presentable)
        {
          model->append(std::string{def.label});
          mapping.push_back(def.field);
        }
      }

      return model;
    }
  }

  TrackCustomViewDialog::TrackCustomViewDialog(Gtk::Window& parent,
                                               rt::TrackPresentationSpec const& initialSpec,
                                               std::string_view initialLabel)
  {
    set_title("Edit Custom View");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(kDefaultWidth, kDefaultHeight);

    setupUi();
    populateFromSpec(initialSpec, initialLabel);
  }

  void TrackCustomViewDialog::setupUi()
  {
    auto* const contentArea = get_content_area();
    contentArea->add_css_class("ao-custom-view-editor");

    auto* mainBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    mainBox->add_css_class("ao-custom-view-main-box");

    // Name
    auto* nameBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    nameBox->add_css_class("ao-custom-view-row");
    auto* nameLabel = Gtk::make_managed<Gtk::Label>("Name");
    nameLabel->set_halign(Gtk::Align::START);
    nameBox->append(*nameLabel);
    _nameEntry.set_hexpand(true);
    nameBox->append(_nameEntry);
    mainBox->append(*nameBox);

    // Group By
    auto* groupBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    groupBox->add_css_class("ao-custom-view-row");
    auto* groupLabel = Gtk::make_managed<Gtk::Label>("Group By");
    groupLabel->set_halign(Gtk::Align::START);
    groupBox->append(*groupLabel);

    auto groupModel = Gtk::StringList::create({});
    _availableGroupKeys = {
      rt::TrackGroupKey::None,
      rt::TrackGroupKey::Artist,
      rt::TrackGroupKey::Album,
      rt::TrackGroupKey::AlbumArtist,
      rt::TrackGroupKey::Genre,
      rt::TrackGroupKey::Composer,
      rt::TrackGroupKey::Work,
      rt::TrackGroupKey::Year,
    };

    for (auto const key : _availableGroupKeys)
    {
      groupModel->append(std::string{groupKeyName(key)});
    }

    _groupDropdown.set_model(groupModel);
    _groupDropdown.set_hexpand(true);
    groupBox->append(_groupDropdown);
    mainBox->append(*groupBox);

    // Sort Terms (using boxed list style)
    auto* sortLabel = Gtk::make_managed<Gtk::Label>("Sort Order");
    sortLabel->set_halign(Gtk::Align::START);
    sortLabel->add_css_class("ao-custom-view-section-title");
    mainBox->append(*sortLabel);

    _sortTermsList.add_css_class("boxed-list");
    _sortTermsList.add_css_class("ao-custom-view-list");
    _sortTermsList.set_selection_mode(Gtk::SelectionMode::NONE);
    mainBox->append(_sortTermsList);

    auto* addSortBtn = Gtk::make_managed<Gtk::Button>("Add Sort Field");
    addSortBtn->set_halign(Gtk::Align::START);
    addSortBtn->signal_clicked().connect(
      [this]
      {
        _sortState.push_back({rt::TrackSortField::Title, true});
        rebuildSortList();
      });
    mainBox->append(*addSortBtn);

    // Visible Fields
    auto* fieldsLabel = Gtk::make_managed<Gtk::Label>("Visible Columns");
    fieldsLabel->set_halign(Gtk::Align::START);
    fieldsLabel->add_css_class("ao-custom-view-section-title");
    mainBox->append(*fieldsLabel);

    _visibleFieldsList.add_css_class("boxed-list");
    _visibleFieldsList.add_css_class("ao-custom-view-list");
    _visibleFieldsList.set_selection_mode(Gtk::SelectionMode::NONE);
    mainBox->append(_visibleFieldsList);

    auto* addVisibleBtn = Gtk::make_managed<Gtk::Button>("Add Column");
    addVisibleBtn->set_halign(Gtk::Align::START);
    addVisibleBtn->signal_clicked().connect(
      [this]
      {
        _visibleFieldsState.push_back(rt::TrackField::Title);
        rebuildVisibleFieldsList();
      });
    mainBox->append(*addVisibleBtn);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_vexpand(true);
    scroll->set_child(*mainBox);
    contentArea->append(*scroll);

    // Buttons
    _saveButton.add_css_class("suggested-action");
    add_action_widget(_saveButton, Gtk::ResponseType::OK);

    auto* cancelButton = Gtk::make_managed<Gtk::Button>("Cancel");
    add_action_widget(*cancelButton, Gtk::ResponseType::CANCEL);
  }

  void TrackCustomViewDialog::rebuildSortList()
  {
    while (auto* child = _sortTermsList.get_first_child())
    {
      _sortTermsList.remove(*child);
    }

    for (std::size_t i = 0; i < _sortState.size(); ++i)
    {
      auto const& term = _sortState[i];
      auto* const row = Gtk::make_managed<Gtk::ListBoxRow>();
      auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kBoxSpacing);

      auto* const dropdown = Gtk::make_managed<Gtk::DropDown>(createSortFieldsModel(_availableSortFields));

      // Find the index in our mapping
      auto const it = std::ranges::find(_availableSortFields, term.field);
      auto const index = (it != _availableSortFields.end())
                           ? static_cast<::guint>(std::ranges::distance(_availableSortFields.begin(), it))
                           : 0;

      dropdown->set_selected(index);
      dropdown->property_selected().signal_changed().connect(
        [this, i, dropdown]
        {
          if (auto const selected = dropdown->get_selected(); selected < _availableSortFields.size())
          {
            _sortState[i].field = _availableSortFields[selected];
          }
        });
      box->append(*dropdown);

      auto* ascBtn = Gtk::make_managed<Gtk::ToggleButton>("Ascending");
      ascBtn->set_active(term.ascending);

      ascBtn->signal_toggled().connect([this, i, ascBtn] { _sortState[i].ascending = ascBtn->get_active(); });
      box->append(*ascBtn);

      auto* spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      spacer->set_hexpand(true);
      box->append(*spacer);

      auto* upBtn = Gtk::make_managed<Gtk::Button>("Up");
      upBtn->set_sensitive(i > 0);

      upBtn->signal_clicked().connect(
        [this, i]
        {
          std::swap(_sortState[i], _sortState[i - 1]);
          rebuildSortList();
        });
      box->append(*upBtn);

      auto* downBtn = Gtk::make_managed<Gtk::Button>("Down");
      downBtn->set_sensitive(i < _sortState.size() - 1);

      downBtn->signal_clicked().connect(
        [this, i]
        {
          std::swap(_sortState[i], _sortState[i + 1]);
          rebuildSortList();
        });
      box->append(*downBtn);

      auto* removeBtn = Gtk::make_managed<Gtk::Button>("Remove");
      removeBtn->signal_clicked().connect(
        [this, i]
        {
          _sortState.erase(_sortState.begin() + static_cast<std::ptrdiff_t>(i));
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

    for (std::size_t i = 0; i < _visibleFieldsState.size(); ++i)
    {
      auto const field = _visibleFieldsState[i];
      auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
      auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kBoxSpacing);

      auto* dropdown = Gtk::make_managed<Gtk::DropDown>(createVisibleFieldsModel(_availableVisibleFields));

      auto const it = std::ranges::find(_availableVisibleFields, field);
      auto const index = (it != _availableVisibleFields.end())
                           ? static_cast<::guint>(std::ranges::distance(_availableVisibleFields.begin(), it))
                           : 0;

      dropdown->set_selected(index);
      dropdown->property_selected().signal_changed().connect(
        [this, i, dropdown]
        {
          if (auto const selected = dropdown->get_selected(); selected < _availableVisibleFields.size())
          {
            _visibleFieldsState[i] = _availableVisibleFields[selected];
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
          std::swap(_visibleFieldsState[i], _visibleFieldsState[i - 1]);
          rebuildVisibleFieldsList();
        });
      box->append(*upBtn);

      auto* downBtn = Gtk::make_managed<Gtk::Button>("Down");
      downBtn->set_sensitive(i < _visibleFieldsState.size() - 1);

      downBtn->signal_clicked().connect(
        [this, i]
        {
          std::swap(_visibleFieldsState[i], _visibleFieldsState[i + 1]);
          rebuildVisibleFieldsList();
        });
      box->append(*downBtn);

      auto* removeBtn = Gtk::make_managed<Gtk::Button>("Remove");
      removeBtn->signal_clicked().connect(
        [this, i]
        {
          _visibleFieldsState.erase(_visibleFieldsState.begin() + static_cast<std::ptrdiff_t>(i));
          rebuildVisibleFieldsList();
        });
      box->append(*removeBtn);

      row->set_child(*box);
      _visibleFieldsList.append(*row);
    }
  }

  void TrackCustomViewDialog::populateFromSpec(rt::TrackPresentationSpec const& spec, std::string_view label)
  {
    _nameEntry.set_text(std::string{label});

    if (auto const it = std::ranges::find(_availableGroupKeys, spec.groupBy); it != _availableGroupKeys.end())
    {
      _groupDropdown.set_selected(static_cast<::guint>(std::ranges::distance(_availableGroupKeys.begin(), it)));
    }

    _sortState.clear();

    for (auto const& term : spec.sortBy)
    {
      _sortState.push_back(term);
    }

    rebuildSortList();

    _visibleFieldsState.clear();

    for (auto const field : spec.visibleFields)
    {
      _visibleFieldsState.push_back(field);
    }

    rebuildVisibleFieldsList();
  }

  rt::CustomTrackPresentationPreset TrackCustomViewDialog::collectState() const
  {
    auto state = rt::CustomTrackPresentationPreset{};
    state.spec.id = generateId();
    state.label = _nameEntry.get_text();

    if (auto const groupIndex = _groupDropdown.get_selected(); groupIndex < _availableGroupKeys.size())
    {
      state.spec.groupBy = _availableGroupKeys[groupIndex];
    }
    else
    {
      state.spec.groupBy = rt::TrackGroupKey::None;
    }

    state.spec.sortBy = _sortState;
    state.spec.visibleFields = _visibleFieldsState;

    return state;
  }

  std::optional<TrackCustomViewDialog::Result> TrackCustomViewDialog::runDialog()
  {
    show();

    auto loop = Glib::MainLoop::create(false);
    auto response = Gtk::ResponseType::CANCEL;

    signal_response().connect(
      [&loop, &response](std::int32_t resp)
      {
        response = static_cast<Gtk::ResponseType>(resp);
        loop->quit();
      });

    loop->run();
    hide();

    if (response == Gtk::ResponseType::OK)
    {
      return Result{.state = collectState(), .deleted = false};
    }

    return std::nullopt;
  }
} // namespace ao::gtk

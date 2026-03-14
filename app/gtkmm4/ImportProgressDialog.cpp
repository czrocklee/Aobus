#include "ImportProgressDialog.h"

ImportProgressDialog::ImportProgressDialog(int maxItems, Gtk::Window& parent) : Gtk::Dialog(), _maxItems(maxItems)
{
  set_title("Importing Music");
  set_transient_for(parent);
  set_modal(true);
  setupUi(maxItems);
}

ImportProgressDialog::~ImportProgressDialog() = default;

void ImportProgressDialog::setupUi([[maybe_unused]] int maxItems)
{
  set_default_size(400, 150);

  auto box = Gtk::Box(Gtk::Orientation::VERTICAL, 8);
  box.set_margin(12);

  _progressLabel.set_text("Preparing to import...");
  _progressLabel.set_halign(Gtk::Align::START);
  box.append(_progressLabel);

  _progressBar.set_fraction(0.0);
  box.append(_progressBar);

  _okButton.set_label("OK");
  _okButton.set_sensitive(false);
  _okButton.signal_clicked().connect([this]() { response(Gtk::ResponseType::OK); });
  box.append(_okButton);

  // Add action area for the button
  auto actionArea = Gtk::Box(Gtk::Orientation::HORIZONTAL, 6);
  actionArea.set_halign(Gtk::Align::END);
  actionArea.append(_okButton);
  actionArea.set_margin(12);
  box.append(actionArea);

  set_child(box);
}

void ImportProgressDialog::onNewTrack(const std::string& path, int itemIndex)
{
  auto fraction = static_cast<double>(itemIndex) / _maxItems;
  _progressBar.set_fraction(fraction);
  _progressLabel.set_text("Importing: " + path);
}

void ImportProgressDialog::ready()
{
  _progressBar.set_fraction(1.0);
  _progressLabel.set_text("Import complete!");
  _okButton.set_sensitive(true);
}

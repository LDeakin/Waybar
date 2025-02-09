#pragma once

#include <gtkmm/button.h>
#include <json/value.h>

#include "AModule.hpp"
#include "bar.hpp"
#include "modules/niri/backend.hpp"

namespace waybar::modules::niri {

class Taskbar;

class Task {
 public:
  Task(const Json::Value &window, const Taskbar &taskbar);

  Gtk::Button button;

 private:
  const Taskbar &taskbar_;
  Gtk::Box content;
  Gtk::Image icon;
};

class Taskbar : public AModule, public EventHandler {
 public:
  Taskbar(const std::string &, const Bar &, const Json::Value &);
  ~Taskbar() override;
  void update() override;

  const Json::Value& config() const;
  const std::vector<Glib::RefPtr<Gtk::IconTheme>> &icon_themes() const;

 private:
  void onEvent(const Json::Value &ev) override;
  void doUpdate();
  Task &addButton(const Json::Value &ws);

  const Bar &bar_;
  Gtk::Box box_;
  // Map from niri window id to button.
  std::unordered_map<uint64_t, std::unique_ptr<Task>> buttons_;

  std::vector<Glib::RefPtr<Gtk::IconTheme>> icon_themes_;
};

}  // namespace waybar::modules::niri

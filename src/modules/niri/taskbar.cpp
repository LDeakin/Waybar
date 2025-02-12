#include "modules/niri/taskbar.hpp"

#include <gio/gdesktopappinfo.h>
#include <giomm/desktopappinfo.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <spdlog/spdlog.h>

#include "util/string.hpp"
#include "util/gtk_icon.hpp"


/* Icon loading functions */
static std::vector<std::string> search_prefix() {
  std::vector<std::string> prefixes = {""};

  std::string home_dir = std::getenv("HOME");
  prefixes.push_back(home_dir + "/.local/share/");

  auto xdg_data_dirs = std::getenv("XDG_DATA_DIRS");
  if (!xdg_data_dirs) {
    prefixes.emplace_back("/usr/share/");
    prefixes.emplace_back("/usr/local/share/");
  } else {
    std::string xdg_data_dirs_str(xdg_data_dirs);
    size_t start = 0, end = 0;

    do {
      end = xdg_data_dirs_str.find(':', start);
      auto p = xdg_data_dirs_str.substr(start, end - start);
      prefixes.push_back(trim(p) + "/");

      start = end == std::string::npos ? end : end + 1;
    } while (end != std::string::npos);
  }

  for (auto &p : prefixes) spdlog::debug("Using 'desktop' search path prefix: {}", p);

  return prefixes;
}

static Glib::RefPtr<Gdk::Pixbuf> load_icon_from_file(std::string icon_path, int size) {
  try {
    auto pb = Gdk::Pixbuf::create_from_file(icon_path, size, size);
    return pb;
  } catch (...) {
    return {};
  }
}

static Glib::RefPtr<Gio::DesktopAppInfo> get_app_info_by_name(const std::string &app_id) {
  static std::vector<std::string> prefixes = search_prefix();

  std::vector<std::string> app_folders = {"", "applications/", "applications/kde/",
                                          "applications/org.kde."};

  std::vector<std::string> suffixes = {"", ".desktop"};

  for (auto &prefix : prefixes) {
    for (auto &folder : app_folders) {
      for (auto &suffix : suffixes) {
        auto app_info_ =
            Gio::DesktopAppInfo::create_from_filename(prefix + folder + app_id + suffix);
        if (!app_info_) {
          continue;
        }

        return app_info_;
      }
    }
  }

  return {};
}

Glib::RefPtr<Gio::DesktopAppInfo> get_desktop_app_info(const std::string &app_id) {
  auto app_info = get_app_info_by_name(app_id);
  if (app_info) {
    return app_info;
  }

  std::string desktop_file = "";

  gchar ***desktop_list = g_desktop_app_info_search(app_id.c_str());
  if (desktop_list != nullptr && desktop_list[0] != nullptr) {
    for (size_t i = 0; desktop_list[0][i]; i++) {
      if (desktop_file == "") {
        desktop_file = desktop_list[0][i];
      } else {
        auto tmp_info = Gio::DesktopAppInfo::create(desktop_list[0][i]);
        if (!tmp_info)
          // see https://github.com/Alexays/Waybar/issues/1446
          continue;

        auto startup_class = tmp_info->get_startup_wm_class();
        if (startup_class == app_id) {
          desktop_file = desktop_list[0][i];
          break;
        }
      }
    }
    g_strfreev(desktop_list[0]);
  }
  g_free(desktop_list);

  return get_app_info_by_name(desktop_file);
}


static std::string get_icon_name_from_icon_theme(const Glib::RefPtr<Gtk::IconTheme> &icon_theme,
                                                 const std::string &app_id) {
  if (icon_theme->lookup_icon(app_id, 24)) return app_id;

  return "";
}

bool image_load_icon(Gtk::Image &image, const Glib::RefPtr<Gtk::IconTheme> &icon_theme,
                           Glib::RefPtr<Gio::DesktopAppInfo> app_info, int size) {
  std::string ret_icon_name = "unknown";
  if (app_info) {
    std::string icon_name =
        get_icon_name_from_icon_theme(icon_theme, app_info->get_startup_wm_class());
    if (!icon_name.empty()) {
      ret_icon_name = icon_name;
    } else {
      if (app_info->get_icon()) {
        ret_icon_name = app_info->get_icon()->to_string();
      }
    }
  }

  Glib::RefPtr<Gdk::Pixbuf> pixbuf;
  auto scaled_icon_size = size * image.get_scale_factor();

  try {
    pixbuf = icon_theme->load_icon(ret_icon_name, scaled_icon_size, Gtk::ICON_LOOKUP_FORCE_SIZE);
    spdlog::debug("Loaded icon '{}'", ret_icon_name);
  } catch (...) {
    if (Glib::file_test(ret_icon_name, Glib::FILE_TEST_EXISTS)) {
      pixbuf = load_icon_from_file(ret_icon_name, scaled_icon_size);
      spdlog::debug("Loaded icon from file '{}'", ret_icon_name);
    } else {
      try {
        pixbuf = DefaultGtkIconThemeWrapper::load_icon(
            "image-missing", scaled_icon_size, Gtk::IconLookupFlags::ICON_LOOKUP_FORCE_SIZE);
        spdlog::debug("Loaded icon from resource");
      } catch (...) {
        pixbuf = {};
        spdlog::debug("Unable to load icon.");
      }
    }
  }

  if (pixbuf) {
    if (pixbuf->get_width() != scaled_icon_size) {
      int width = scaled_icon_size * pixbuf->get_width() / pixbuf->get_height();
      pixbuf = pixbuf->scale_simple(width, scaled_icon_size, Gdk::InterpType::INTERP_BILINEAR);
    }
    auto surface = Gdk::Cairo::create_surface_from_pixbuf(pixbuf, image.get_scale_factor(),
                                                          image.get_window());
    image.set(surface);
    return true;
  }

  return false;
}

namespace waybar::modules::niri {

Taskbar::Taskbar(const std::string &id, const Bar &bar, const Json::Value &config)
    : AModule(config, "taskbar", id, false, false), bar_(bar), box_(bar.orientation, 0) {
  box_.set_name("taskbar");
  if (!id.empty()) {
    box_.get_style_context()->add_class(id);
  }
  box_.get_style_context()->add_class(MODULE_CLASS);
  event_box_.add(box_);

  /* Get the configured icon theme if specified */
  if (config_["icon-theme"].isArray()) {
    for (auto &c : config_["icon-theme"]) {
      auto it_name = c.asString();

      auto it = Gtk::IconTheme::create();
      it->set_custom_theme(it_name);
      spdlog::debug("Use custom icon theme: {}", it_name);

      icon_themes_.push_back(it);
    }
  } else if (config_["icon-theme"].isString()) {
    auto it_name = config_["icon-theme"].asString();

    auto it = Gtk::IconTheme::create();
    it->set_custom_theme(it_name);
    spdlog::debug("Use custom icon theme: {}", it_name);

    icon_themes_.push_back(it);
  }

  icon_themes_.push_back(Gtk::IconTheme::get_default());

  if (!gIPC) gIPC = std::make_unique<IPC>();

  gIPC->registerForIPC("WorkspacesChanged", this);
  gIPC->registerForIPC("WorkspaceActivated", this);
  gIPC->registerForIPC("WorkspaceActiveWindowChanged", this);
  gIPC->registerForIPC("WindowsChanged", this);
  gIPC->registerForIPC("WindowOpenedOrChanged", this);
  gIPC->registerForIPC("WindowClosed", this);
  gIPC->registerForIPC("WindowFocusChanged", this);

  dp.emit();
}

Taskbar::~Taskbar() { gIPC->unregisterForIPC(this); }

void Taskbar::onEvent(const Json::Value &ev) { dp.emit(); }

void Taskbar::doUpdate() {
  auto ipcLock = gIPC->lockData();

  const auto alloutputs = config_["all-outputs"].asBool();
  std::vector<Json::Value> my_windows;
  const auto &windows = gIPC->windows();
  const auto &workspaces = gIPC->workspaces();

  const auto separateOutputs = config_["separate-outputs"].asBool();
  const auto ws_it = std::find_if(workspaces.cbegin(), workspaces.cend(), [&](const auto &ws) {
    if (separateOutputs) {
      return ws["is_active"].asBool() && ws["output"].asString() == bar_.output->name;
    }

    return ws["is_focused"].asBool();
  });

  std::vector<Json::Value>::const_iterator it;
  if (ws_it == workspaces.cend() || (*ws_it)["active_window_id"].isNull()) {
    it = windows.cend();
  } else {
    const auto id = (*ws_it)["active_window_id"].asUInt64();
    it = std::find_if(windows.cbegin(), windows.cend(),
                      [id](const auto &win) { return win["id"].asUInt64() == id; });
  }

  std::copy_if(windows.cbegin(), windows.cend(), std::back_inserter(my_windows),
               [&](const auto &window) {
                 // workspace["output"].asString() == bar_.output->name
                 return window["workspace_id"] == (*ws_it)["id"];
               });

  // Remove buttons for removed windows.
  for (auto it = buttons_.begin(); it != buttons_.end();) {
    auto window = std::find_if(my_windows.begin(), my_windows.end(),
                           [it](const auto &window) { return window["id"].asUInt64() == it->first; });
    if (window == my_windows.end()) {
      it = buttons_.erase(it);
    } else {
      ++it;
    }
  }

  // Add buttons for new windows, update existing ones.
  for (const auto &window : my_windows) {
    auto bit = buttons_.find(window["id"].asUInt64());
    auto &task = bit == buttons_.end() ? addButton(window) : *(bit->second);
    auto &button = task.button;
    auto style_context = button.get_style_context();

    const auto title = window["title"].asString();

    if (tooltipEnabled()) button.set_tooltip_text(title);

    if (window["is_focused"].asBool())
      style_context->add_class("focused");
    else
      style_context->remove_class("focused");

    if (window["is_floating"].asBool())
      style_context->add_class("floating");
    else
      style_context->remove_class("floating");

    std::string name;
    if (window["name"]) {
      name = window["name"].asString();
    } else {
      name = std::to_string(window["idx"].asUInt());
    }
    button.set_name("niri-taskbar-" + name);
  }

  // Refresh the button order.
  for (auto it = my_windows.cbegin(); it != my_windows.cend(); ++it) {
    const auto &window = *it;

    auto pos = window["idx"].asUInt() - 1;
    if (alloutputs) pos = it - my_windows.cbegin();

    auto &task = *buttons_[window["id"].asUInt64()];
    box_.reorder_child(task.button, pos);
  }
}

void Taskbar::update() {
  doUpdate();
  AModule::update();
}


const Json::Value& Taskbar::config() const { return config_; }
const std::vector<Glib::RefPtr<Gtk::IconTheme>> &Taskbar::icon_themes() const { return icon_themes_; }

Task::Task(const Json::Value &window, const Taskbar &taskbar): taskbar_(taskbar) {
  const auto& config = taskbar_.config();

  button.set_relief(Gtk::RELIEF_NONE);
  if (!config["disable-click"].asBool()) {
    const auto id = window["id"].asUInt64();
    button.signal_pressed().connect([=] {
      try {
        // {"Action":{"FocusWindow":{"id":1}}}
        Json::Value request(Json::objectValue);
        auto &action = (request["Action"] = Json::Value(Json::objectValue));
        auto &focusWorkspace = (action["FocusWindow"] = Json::Value(Json::objectValue));
        focusWorkspace["id"] = id;

        IPC::send(request);
      } catch (const std::exception &e) {
        spdlog::error("Error switching window: {}", e.what());
      }
    });
  }

  if (window["app_id"]) {
    const auto &app_id = window["app_id"].asString();
    auto app_info = get_desktop_app_info(app_id);
    int icon_size = config["icon-size"].isInt() ? config["icon-size"].asInt() : 16;
    bool found = false;
    for (auto &icon_theme : taskbar_.icon_themes()) {
      if (image_load_icon(icon, icon_theme, app_info, icon_size)) {
        found = true;
        break;
      }
    }
    if (found) {
      content.add(icon);
      content.show();
      button.add(content);
      icon.show();
      button.show();
    }
  }
}

Task &Taskbar::addButton(const Json::Value &window) {
  auto pair = buttons_.emplace(window["id"].asUInt64(), std::make_unique<Task>(window, *this));
  auto &task = *pair.first->second;
  box_.pack_start(task.button, false, false, 0);
  return task;
}

}

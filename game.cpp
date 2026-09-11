#include "game.hpp"

#include "assets.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

using namespace blit;

namespace {

CitsyBlitHost g_host;
std::optional<citsy::Engine> g_engine;

AppMode g_mode = AppMode::Picker;
std::string g_error;
std::vector<GameEntry> g_games;
int g_selected = 0;
uint32_t g_last_time = 0;

[[nodiscard]] bool has_bitsy_ext(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name.size() >= 6 && name.substr(name.size() - 6) == ".bitsy";
}

[[nodiscard]] std::string read_file_text(const std::string& path) {
    File f(path);
    if (f.is_open()) {
        const uint32_t len = f.get_length();
        std::string text(len, '\0');
        if (len > 0) {
            f.read(0, len, text.data());
        }
        return text;
    }

    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

void add_game(const std::string& path, const std::string& label, bool bundled) {
    for (const auto& g : g_games) {
        if (g.path == path) return;
    }
    g_games.push_back({path, label, bundled});
}

void scan_directory(const std::string& dir) {
    auto files = list_files(dir, [](const FileInfo& info) {
        if (info.flags & FileFlags::directory) return false;
        return has_bitsy_ext(info.name);
    });
    for (const auto& info : files) {
        std::string path = info.name;
        if (!dir.empty() && dir != "/" && dir != ".") {
            path = dir;
            if (path.back() != '/') path += '/';
            path += info.name;
        }
        add_game(path, info.name, false);
    }
}

void scan_games() {
    g_games.clear();
    g_selected = 0;

    File::add_buffer_file("mossland.bitsy", asset_mossland_bitsy, asset_mossland_bitsy_length);

    add_game("mossland.bitsy", "Mossland", true);

    scan_directory("");
    scan_directory(".");
    scan_directory("games");

    if (g_games.empty()) {
        g_error = "No .bitsy files found.";
        g_mode = AppMode::Error;
    }
}

bool start_game(const std::string& path) {
    const std::string text = read_file_text(path);
    if (text.empty()) {
        g_error = "Could not read:\n" + path;
        g_mode = AppMode::Error;
        g_engine.reset();
        return false;
    }

    try {
        g_engine.emplace(text);
        g_engine->start(g_host);
        g_mode = AppMode::Playing;
        g_error.clear();
        return true;
    } catch (const citsy::ParseError& e) {
        g_error = std::string("Parse error:\n") + e.what();
    } catch (const std::exception& e) {
        g_error = std::string("Load failed:\n") + e.what();
    }
    g_engine.reset();
    g_mode = AppMode::Error;
    return false;
}

void stop_audio() {
    channels[0].off();
    channels[1].off();
}

void return_to_picker() {
    g_engine.reset();
    stop_audio();
    g_mode = AppMode::Picker;
    g_error.clear();
}

void draw_picker() {
    screen.pen = Pen(12, 10, 18);
    screen.clear();

    screen.pen = Pen(255, 255, 255);
    screen.text("citsy", minimal_font, Point(8, 6));
    screen.pen = Pen(180, 180, 200);
    screen.text("Select a .bitsy game", minimal_font, Point(8, 18));

    const int line_h = 12;
    const int top = 36;
    const int visible = (screen.bounds.h - top - 24) / line_h;
    int start = 0;
    if (g_selected >= visible) {
        start = g_selected - visible + 1;
    }

    for (int i = 0; i < visible && start + i < static_cast<int>(g_games.size()); ++i) {
        const int idx = start + i;
        const int y = top + i * line_h;
        if (idx == g_selected) {
            screen.pen = Pen(70, 90, 160);
            screen.rectangle(Rect(4, y - 1, screen.bounds.w - 8, line_h));
            screen.pen = Pen(255, 255, 255);
        } else {
            screen.pen = Pen(200, 200, 210);
        }
        screen.text(g_games[static_cast<size_t>(idx)].label, minimal_font, Point(10, y));
    }

    screen.pen = Pen(140, 140, 160);
    screen.text("A start   D-pad move   MENU back", minimal_font,
                Point(8, screen.bounds.h - 14));
}

void draw_error() {
    screen.pen = Pen(40, 8, 8);
    screen.clear();
    screen.pen = Pen(255, 220, 220);
    screen.text("Error", minimal_font, Point(8, 8));
    screen.pen = Pen(255, 255, 255);
    screen.text(g_error, minimal_font, Rect(8, 24, screen.bounds.w - 16, screen.bounds.h - 48));
    screen.pen = Pen(200, 180, 180);
    screen.text("A / MENU to go back", minimal_font, Point(8, screen.bounds.h - 14));
}

} // namespace

void init() {
    set_screen_mode(ScreenMode::hires);
    scan_games();

    const char* launch = get_launch_path();
    if (launch && launch[0] != '\0') {
        start_game(launch);
        return;
    }

    if (g_mode != AppMode::Error && !g_games.empty()) {
        start_game(g_games.front().path);
    }
}

void render(uint32_t) {
    screen.alpha = 255;
    screen.mask = nullptr;

    switch (g_mode) {
        case AppMode::Playing:
            g_host.draw();
            break;
        case AppMode::Error:
            draw_error();
            break;
        case AppMode::Picker:
        default:
            draw_picker();
            break;
    }
}

void update(uint32_t time) {
    const double dt = (g_last_time == 0)
        ? 16.667
        : static_cast<double>(time - g_last_time);
    g_last_time = time;

    if (g_mode == AppMode::Playing && g_engine) {
        if (buttons.released & Button::MENU) {
            return_to_picker();
            return;
        }
        if (!g_engine->is_running()) {
            return_to_picker();
            return;
        }
        g_host.set_delta(dt);
        g_host.poll_input();
        g_engine->update(g_host);
        g_host.play_audio();
        return;
    }

    if (g_mode == AppMode::Error) {
        if (buttons.released & (Button::A | Button::MENU)) {
            return_to_picker();
        }
        return;
    }

    if (g_games.empty()) return;

    if (buttons.pressed & Button::DPAD_UP) {
        g_selected = (g_selected + static_cast<int>(g_games.size()) - 1)
            % static_cast<int>(g_games.size());
    }
    if (buttons.pressed & Button::DPAD_DOWN) {
        g_selected = (g_selected + 1) % static_cast<int>(g_games.size());
    }
    if (buttons.released & Button::A) {
        start_game(g_games[static_cast<size_t>(g_selected)].path);
    }
}

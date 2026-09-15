#include "game.hpp"

#include "assets.hpp"
#include "hardware.hpp"
#include "host.hpp"
#include "launcher.hpp"

#include <citsy/engine.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace blit;

namespace {

constexpr int kMaxGames = 32;

CitsyBlitHost g_host;
std::optional<citsy::Engine> g_engine;
HardwareStatus g_hw;
LauncherUI g_ui;

SystemState g_state = SystemState::MainMenu;
std::string g_error;
std::vector<GameEntry> g_games;
std::string g_current_path;
std::string g_current_label;

int g_selected = 0;
int g_settings_row = 0;
int g_pause_row = 0;
uint32_t g_last_time = 0;

struct HoldRepeat {
    uint32_t next_ms = 0;
    bool armed = false;

    bool tick(bool down, uint32_t time, uint32_t delay_ms = 220, uint32_t rate_ms = 85) {
        if (!down) {
            armed = false;
            return false;
        }
        if (!armed) {
            armed = true;
            next_ms = time + delay_ms;
            return true;
        }
        if (int32_t(time - next_ms) >= 0) {
            next_ms = time + rate_ms;
            return true;
        }
        return false;
    }
};

HoldRepeat g_repeat_up;
HoldRepeat g_repeat_down;
HoldRepeat g_repeat_left;
HoldRepeat g_repeat_right;

void reset_repeats() {
    g_repeat_up = {};
    g_repeat_down = {};
    g_repeat_left = {};
    g_repeat_right = {};
}

[[nodiscard]] bool has_bitsy_ext(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name.size() >= 6 && name.substr(name.size() - 6) == ".bitsy";
}

[[nodiscard]] std::string strip_markup(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] == '{') {
            const auto end = in.find('}', i);
            if (end == std::string_view::npos) {
                out.append(in.substr(i));
                break;
            }
            i = end + 1;
        } else {
            out.push_back(in[i++]);
        }
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
        out.pop_back();
    }
    return out;
}

[[nodiscard]] bool is_bitsy_keyword(std::string_view line) {
    auto first = line.substr(0, line.find(' '));
    return first == "PAL" || first == "ROOM" || first == "SET" || first == "TIL" ||
           first == "SPR" || first == "ITM" || first == "DLG" || first == "END" ||
           first == "VAR" || first == "FONT" || first == "NAME" || first == "EXT";
}

[[nodiscard]] std::string extract_title(std::string_view text, std::string_view fallback) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto nl = text.find('\n', i);
        const auto end = nl == std::string_view::npos ? text.size() : nl;
        auto line = text.substr(i, end - i);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        i = (nl == std::string_view::npos) ? text.size() : nl + 1;

        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
            line.remove_prefix(1);
        }
        if (line.empty() || line.front() == '#' || line.front() == '!') continue;
        if (is_bitsy_keyword(line)) break;

        auto title = strip_markup(line);
        if (!title.empty()) return title;
    }
    return std::string(fallback);
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
    if (static_cast<int>(g_games.size()) >= kMaxGames) return;
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
        const std::string text = read_file_text(path);
        std::string fallback = info.name;
        if (fallback.size() > 6) fallback.resize(fallback.size() - 6);
        add_game(path, extract_title(text, fallback), false);
    }
}

void scan_games() {
    g_games.clear();
    g_selected = 0;

    File::add_buffer_file("mossland.bitsy", asset_mossland_bitsy, asset_mossland_bitsy_length);
    File::add_buffer_file("sandbox.bitsy", asset_sandbox_bitsy, asset_sandbox_bitsy_length);

    add_game("mossland.bitsy", "Mossland", true);
    add_game("sandbox.bitsy", "Sandbox", true);

    scan_directory("");
    scan_directory(".");
    scan_directory("games");
}

[[nodiscard]] int menu_rows() {
    return static_cast<int>(g_games.size()) + 1;
}

void enter_main_menu() {
    g_engine.reset();
    g_host.stop_audio();
    g_state = SystemState::MainMenu;
    g_error.clear();
    reset_repeats();
}

bool start_game(const std::string& path, const std::string& label) {
    const std::string text = read_file_text(path);
    if (text.empty()) {
        g_error = "Could not read:\n" + path;
        g_state = SystemState::Error;
        g_engine.reset();
        return false;
    }

    try {
        g_engine.emplace(text);
        g_engine->start(g_host);
        g_current_path = path;
        g_current_label = label;
        g_state = SystemState::GameRunning;
        g_error.clear();
        g_pause_row = 0;
        reset_repeats();
        return true;
    } catch (const citsy::ParseError& e) {
        g_error = std::string("Parse error:\n") + e.what();
    } catch (const std::exception& e) {
        g_error = std::string("Load failed:\n") + e.what();
    }
    g_engine.reset();
    g_state = SystemState::Error;
    return false;
}

void open_pause() {
    g_host.stop_audio();
    g_pause_row = 0;
    g_state = SystemState::PauseOverlay;
    reset_repeats();
    g_ui.notify_press();
}

void resume_game() {
    g_state = SystemState::GameRunning;
}

void restart_game() {
    if (g_current_path.empty()) {
        enter_main_menu();
        return;
    }
    start_game(g_current_path, g_current_label);
}

void activate_menu_item() {
    const int n = static_cast<int>(g_games.size());
    if (g_selected == n) {
        g_settings_row = 0;
        g_state = SystemState::Settings;
        reset_repeats();
        g_ui.notify_press();
        return;
    }
    if (g_selected >= 0 && g_selected < n) {
        const auto& g = g_games[static_cast<std::size_t>(g_selected)];
        start_game(g.path, g.label);
        g_ui.notify_press();
    }
}

void move_menu(int delta) {
    const int rows = menu_rows();
    if (rows <= 0) return;
    g_selected = (g_selected + delta + rows) % rows;
    g_ui.notify_move();
}

void update_main_menu(uint32_t time) {
    if (g_repeat_up.tick(buttons & Button::DPAD_UP, time)) move_menu(-1);
    if (g_repeat_down.tick(buttons & Button::DPAD_DOWN, time)) move_menu(1);

    if (buttons.released & Button::A) {
        activate_menu_item();
    }
    if (buttons.released & (Button::X | Button::MENU)) {
        g_settings_row = 0;
        g_state = SystemState::Settings;
        reset_repeats();
        g_ui.notify_press();
    }
}

void play_volume_chirp() {
    if (g_hw.volume() <= 0) {
        channels[0].off();
        return;
    }
    auto& ch = channels[0];
    ch.waveforms = Waveform::SQUARE;
    ch.frequency = 880;
    ch.volume = 0xffff;
    ch.pulse_width = 0x7fff;
    ch.attack_ms = 8;
    ch.decay_ms = 40;
    ch.sustain = 0;
    ch.release_ms = 1;
    ch.trigger_attack();
}

void update_settings(uint32_t time) {
    if (g_repeat_up.tick(buttons & Button::DPAD_UP, time)) {
        g_settings_row = (g_settings_row + 1) % 2;
        g_ui.notify_move();
    }
    if (g_repeat_down.tick(buttons & Button::DPAD_DOWN, time)) {
        g_settings_row = (g_settings_row + 1) % 2;
        g_ui.notify_move();
    }

    auto nudge = [&](int dir) {
        if (g_settings_row == 0) {
            g_hw.set_volume(g_hw.volume() + dir);
            play_volume_chirp();
        } else {
            g_hw.set_brightness(g_hw.brightness() + dir);
        }
        g_ui.notify_move();
    };

    if (g_repeat_left.tick(buttons & Button::DPAD_LEFT, time)) nudge(-1);
    if (g_repeat_right.tick(buttons & Button::DPAD_RIGHT, time)) nudge(1);

    if (buttons.released & (Button::B | Button::MENU)) {
        g_hw.save();
        g_state = SystemState::MainMenu;
        reset_repeats();
        g_ui.notify_press();
    }
}

void update_pause() {
    if (buttons.pressed & Button::DPAD_UP) {
        g_pause_row = (g_pause_row + 2) % 3;
        g_ui.notify_move();
    }
    if (buttons.pressed & Button::DPAD_DOWN) {
        g_pause_row = (g_pause_row + 1) % 3;
        g_ui.notify_move();
    }

    const bool confirm = buttons.released & Button::A;
    const bool back = buttons.released & (Button::B | Button::MENU);
    if (back) {
        resume_game();
        return;
    }
    if (!confirm) return;

    g_ui.notify_press();
    switch (g_pause_row) {
        case 0: resume_game(); break;
        case 1: restart_game(); break;
        default: enter_main_menu(); break;
    }
}

void update_game(double dt) {
    if (buttons.released & Button::MENU) {
        open_pause();
        return;
    }
    if (!g_engine || !g_engine->is_running()) {
        enter_main_menu();
        return;
    }
    g_host.set_delta(dt);
    g_host.poll_input();
    g_engine->update(g_host);
    g_host.play_audio();
}

} // namespace

void init() {
    set_screen_mode(ScreenMode::hires);
    g_hw.init();
    scan_games();
    g_host.set_menu_captured(true);

    const char* launch = get_launch_path();
    if (launch && launch[0] != '\0') {
        start_game(launch, launch);
    }
}

void render(uint32_t time) {
    screen.alpha = 255;
    screen.mask = nullptr;
    screen.clip = Rect(0, 0, screen.bounds.w, screen.bounds.h);
    g_ui.update(time);

    switch (g_state) {
        case SystemState::GameRunning:
            g_host.draw();
            break;

        case SystemState::PauseOverlay:
            g_host.draw();
            g_ui.draw_pause_overlay(g_pause_row, g_current_label);
            break;

        case SystemState::Error:
            g_ui.draw_bezel();
            g_ui.draw_error(g_error);
            break;

        case SystemState::Settings:
            g_ui.draw_bezel();
            g_ui.draw_status_bar("SETUP", g_hw, time);
            g_ui.draw_settings(g_hw, g_settings_row);
            break;

        case SystemState::MainMenu:
        default:
            g_ui.draw_bezel();
            g_ui.draw_status_bar("CITSY", g_hw, time);
            g_ui.draw_main_menu(
                g_games.empty() ? nullptr : g_games.data(),
                static_cast<int>(g_games.size()),
                g_selected,
                time);
            break;
    }

    if (g_hw.needs_software_veil()) {
        g_ui.draw_brightness_veil(g_hw.brightness());
    }
}

void update(uint32_t time) {
    const double dt = (g_last_time == 0)
        ? 16.667
        : static_cast<double>(time - g_last_time);
    g_last_time = time;

    g_hw.update(time);
    g_ui.update(time);

    switch (g_state) {
        case SystemState::GameRunning:
            update_game(dt);
            break;
        case SystemState::PauseOverlay:
            update_pause();
            break;
        case SystemState::Settings:
            update_settings(time);
            break;
        case SystemState::Error:
            if (buttons.released & (Button::A | Button::B | Button::MENU)) {
                enter_main_menu();
            }
            break;
        case SystemState::MainMenu:
        default:
            update_main_menu(time);
            break;
    }
}

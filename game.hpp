#pragma once

#include <cstdint>
#include <string>

struct GameEntry {
    std::string path;
    std::string label;
    bool bundled = false;
};

enum class SystemState : uint8_t {
    MainMenu,
    Settings,
    GameRunning,
    PauseOverlay,
    Error,
};

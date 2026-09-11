#pragma once

#include "32blit.hpp"
#include "host.hpp"

#include <citsy/engine.hpp>

#include <optional>
#include <string>
#include <vector>

struct GameEntry {
    std::string path;
    std::string label;
    bool bundled = false;
};

enum class AppMode {
    Picker,
    Playing,
    Error,
};

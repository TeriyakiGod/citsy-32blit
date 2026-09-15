#pragma once

#include <cstdint>

/// Runtime hooks for the patched SSD1351 HAL (display/dbi_ssd1351.cpp).
/// Linked only in Pico/VGC builds.

/// Command 0xC7 master contrast, 0–15. Call from the 32blit tick thread
/// after `init_display()`. Lower values reduce segment current (and PWM
/// harshness) without a software black veil, which cannot change the
/// panel's multiplex rate.
void ssd1351_set_master_contrast(uint8_t level);

// Patched chili-chip SSD1351 DBI/SPI driver.
// Drop-in replacement for 32blit-pico/display/dbi_ssd1351.cpp.
//
// Fixes vs stock HAL:
//   1. 0xB3 CLOCK_DIV 0xF0 (max oscillator, /1) — ~2× OLED PWM refresh.
//   2. DISPLAY_ENHANCE / PRECHARGE_LEVEL / linear LUT that stock skipped.
//   3. Fractional PIO clkdiv so SCK actually reaches LCD_MAX_CLOCK (20 MHz).
//   4. Re-arm column/row + WRITE_RAM every frame (GRAM pointer cannot drift).
//   5. Wait for PIO TX stall, not only DMA complete, before the next burst.
//   6. Optional LCD_TE_PIN / LCD_VSYNC_PIN wait (SSD1351 modules rarely
//      break this pin out; the path matches the ST7789 HAL when wired).

#include "display.hpp"
#include "ssd1351_init_seq.hpp"
#include "ssd1351_tune.hpp"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "pico/binary_info.h"
#include "pico/time.h"

#include "config.h"

#ifdef DBI_8BIT
#include "dbi-8bit.pio.h"
#else
#include "dbi-spi.pio.h"
#endif

#if defined(LCD_TE_PIN) && !defined(LCD_VSYNC_PIN)
#define LCD_VSYNC_PIN LCD_TE_PIN
#endif

using namespace blit;

enum SSD1351 : uint8_t {
    SET_COLUMN        = 0x15,
    SET_ROW           = 0x75,
    WRITE_RAM         = 0x5C,
    READ_RAM          = 0x5D,
    SET_REMAP         = 0xA0,
    START_LINE        = 0xA1,
    DISPLAY_OFFSET    = 0xA2,
    DISPLAY_ALL_OFF   = 0xA4,
    DISPLAY_ALL_ON    = 0xA5,
    NORMAL_DISPLAY    = 0xA6,
    INVERT_DISPLAY    = 0xA7,
    FUNCTION_SELECT   = 0xAB,
    DISPLAY_OFF       = 0xAE,
    DISPLAY_ON        = 0xAF,
    PRECHARGE         = 0xB1,
    DISPLAY_ENHANCE   = 0xB2,
    CLOCK_DIV         = 0xB3,
    SET_VSL           = 0xB4,
    SET_GPIO          = 0xB5,
    PRECHARGE_2       = 0xB6,
    SET_GRAY          = 0xB8,
    USE_LUT           = 0xB9,
    PRECHARGE_LEVEL   = 0xBB,
    VCOMH             = 0xBE,
    CONTRAST_ABC      = 0xC1,
    CONTRAST_MASTER   = 0xC7,
    MUX_RATIO         = 0xCA,
    COMMAND_LOCK      = 0xFD,
    HORIZ_SCROLL      = 0x96,
    STOP_SCROLL       = 0x9E,
    START_SCROLL      = 0x9F,
};

static uint8_t rotation = LCD_ROTATION;

static volatile int buf_index = 0;

static volatile bool do_render = true;

static bool have_vsync = false;
static uint32_t last_render = 0;

static PIO pio = pio0;
static uint pio_sm = 0;
static uint pio_offset = 0, pio_double_offset = 0;

static uint32_t dma_channel = 0;
static bool dma_ready = false;

static uint16_t win_w, win_h;

static bool write_mode = false;
static bool pixel_double = false;
static uint16_t *upd_frame_buffer = nullptr;

static uint16_t *frame_buffer = nullptr;

static volatile int cur_scanline = DISPLAY_HEIGHT;

static void pio_put_byte(PIO pio, uint sm, uint8_t b) {
    while (pio_sm_is_tx_fifo_full(pio, sm));
    *(volatile uint8_t*)&pio->txf[sm] = b;
}

static void pio_wait(PIO pio, uint sm) {
    uint32_t stall_mask = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
    pio->fdebug |= stall_mask;
    while(!(pio->fdebug & stall_mask));
}

static void __isr dbi_dma_irq_handler() {
    if(dma_channel_get_irq0_status(dma_channel)) {
        dma_channel_acknowledge_irq0(dma_channel);

        const int next_line = cur_scanline + 1;
        cur_scanline = next_line;
        if(next_line > win_h / 2)
            return;

        auto count = cur_scanline == (win_h + 1) / 2 ? win_w / 4 : win_w / 2;

        dma_channel_set_trans_count(dma_channel, count, false);
        dma_channel_set_read_addr(dma_channel, upd_frame_buffer + (cur_scanline - 1) * (win_w / 2), true);
    }
}

static bool dma_is_busy() {
    if(pixel_double && cur_scanline <= win_h / 2)
        return true;

    return dma_ready && dma_channel_is_busy(dma_channel);
}

static void wait_for_transfer() {
    if(!dma_ready)
        return;
    dma_channel_wait_for_finish_blocking(dma_channel);
    while(pixel_double && cur_scanline <= win_h / 2) {}
    pio_wait(pio, pio_sm);
}

static void command(uint8_t command, size_t len = 0, const char *data = nullptr) {
    pio_wait(pio, pio_sm);

    if(write_mode) {
        pio_sm_set_enabled(pio, pio_sm, false);
        pio->sm[pio_sm].shiftctrl &= ~PIO_SM0_SHIFTCTRL_PULL_THRESH_BITS;
        pio->sm[pio_sm].shiftctrl |= (8 << PIO_SM0_SHIFTCTRL_PULL_THRESH_LSB) | PIO_SM0_SHIFTCTRL_AUTOPULL_BITS;

        pio_sm_clear_fifos(pio, pio_sm);
        pio_sm_restart(pio, pio_sm);
        pio_sm_set_wrap(pio, pio_sm, pio_offset + dbi_raw_wrap_target, pio_offset + dbi_raw_wrap);
        pio_sm_exec(pio, pio_sm, pio_encode_jmp(pio_offset));

        pio_sm_set_enabled(pio, pio_sm, true);
        write_mode = false;
    }

    gpio_put(LCD_CS_PIN, 0);

    gpio_put(LCD_DC_PIN, 0);
    pio_put_byte(pio, pio_sm, command);

    if(data) {
        pio_wait(pio, pio_sm);
        gpio_put(LCD_DC_PIN, 1);

        for(size_t i = 0; i < len; i++)
            pio_put_byte(pio, pio_sm, data[i]);
    }

    pio_wait(pio, pio_sm);
    gpio_put(LCD_CS_PIN, 1);
}

#define ssd1351_swap(a, b) \
    (((a) ^= (b)), ((b) ^= (a)), ((a) ^= (b)))

static void set_window(uint16_t x1, uint16_t y1, uint16_t w, uint16_t h) {
    uint16_t x2 = x1 + w - 1, y2 = y1 + h - 1;
    if (rotation & 1) {
        ssd1351_swap(x1, y1);
        ssd1351_swap(x2, y2);
    }
    uint8_t cols[2] = { static_cast<uint8_t>(x1 & 0xFF), static_cast<uint8_t>(x2 & 0xFF) };
    uint8_t rows[2] = { static_cast<uint8_t>(y1 & 0xFF), static_cast<uint8_t>(y2 & 0xFF) };
    command(SSD1351::SET_COLUMN, 2, reinterpret_cast<const char *>(cols));
    command(SSD1351::SET_ROW, 2, reinterpret_cast<const char *>(rows));

    win_w = w;
    win_h = h;
}

void set_rotation(uint8_t r) {
    // madctl bits:
    // 6,7 Color depth (01 = 64K)
    // 5   Odd/even split COM (0: disable, 1: enable)
    // 4   Scan direction (0: top-down, 1: bottom-up)
    // 3   Reserved
    // 2   Color remap (0: A->B->C, 1: C->B->A)
    // 1   Column remap (0: 0-127, 1: 127-0)
    // 0   Address increment (0: horizontal, 1: vertical)
    uint8_t madctl = 0b01100000;

    rotation = r & 3;

    switch (rotation) {
    case 0:
        madctl |= 0b00010000;
        win_w = DISPLAY_WIDTH;
        win_h = DISPLAY_HEIGHT;
        break;
    case 1:
        madctl |= 0b00010011;
        win_w = DISPLAY_WIDTH;
        win_h = DISPLAY_HEIGHT;
        break;
    case 2:
        madctl |= 0b00000010;
        win_w = DISPLAY_WIDTH;
        win_h = DISPLAY_HEIGHT;
        break;
    case 3:
        madctl |= 0b00000001;
        win_w = DISPLAY_WIDTH;
        win_h = DISPLAY_HEIGHT;
        break;
    }

    command(SSD1351::SET_REMAP, 1, reinterpret_cast<const char *>(&madctl));
    uint8_t startline = (rotation < 2) ? DISPLAY_HEIGHT-1 : 0;
    command(SSD1351::START_LINE, 1, reinterpret_cast<const char *>(&startline));
}

void enable_display(bool enable) {
    if(enable) {
        command(SSD1351::DISPLAY_ON);
    } else {
        command(SSD1351::DISPLAY_OFF);
    }
}

void invert_display(bool i) {
    if(i) {
        command(SSD1351::INVERT_DISPLAY);
    } else {
        command(SSD1351::NORMAL_DISPLAY);
    }
}

static void send_init_sequence() {
    for(std::size_t i = 0; i < kSsd1351InitSeqCount; ++i) {
        const auto &c = kSsd1351InitSeq[i];
        command(c.cmd, c.nbytes, c.nbytes ? reinterpret_cast<const char *>(c.data) : nullptr);
    }

    set_rotation(rotation);
    invert_display(false);
    set_window(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    command(SSD1351::DISPLAY_ON);
}

static void prepare_write() {
    pio_wait(pio, pio_sm);

    uint8_t r = SSD1351::WRITE_RAM;
    gpio_put(LCD_CS_PIN, 0);

    gpio_put(LCD_DC_PIN, 0);
    pio_put_byte(pio, pio_sm, r);
    pio_wait(pio, pio_sm);

    gpio_put(LCD_DC_PIN, 1);

    pio_sm_set_enabled(pio, pio_sm, false);
    pio_sm_clear_fifos(pio, pio_sm);
    pio_sm_restart(pio, pio_sm);

    if(pixel_double) {
        pio_sm_set_wrap(pio, pio_sm, pio_double_offset + dbi_pixel_double_wrap_target, pio_double_offset + dbi_pixel_double_wrap);
        pio->sm[pio_sm].shiftctrl &= ~(PIO_SM0_SHIFTCTRL_PULL_THRESH_BITS | PIO_SM0_SHIFTCTRL_AUTOPULL_BITS);
        pio_sm_exec(pio, pio_sm, pio_encode_jmp(pio_double_offset));

        dma_channel_hw_addr(dma_channel)->al1_ctrl &= ~DMA_CH0_CTRL_TRIG_DATA_SIZE_BITS;
        dma_channel_hw_addr(dma_channel)->al1_ctrl |= DMA_SIZE_32 << DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB;
    } else {
        pio->sm[pio_sm].shiftctrl &= ~PIO_SM0_SHIFTCTRL_PULL_THRESH_BITS;
        pio->sm[pio_sm].shiftctrl |= (16 << PIO_SM0_SHIFTCTRL_PULL_THRESH_LSB) | PIO_SM0_SHIFTCTRL_AUTOPULL_BITS;

        dma_channel_hw_addr(dma_channel)->al1_ctrl &= ~DMA_CH0_CTRL_TRIG_DATA_SIZE_BITS;
        dma_channel_hw_addr(dma_channel)->al1_ctrl |= DMA_SIZE_16 << DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB;
    }

    pio_sm_set_enabled(pio, pio_sm, true);

    write_mode = true;
}

static void update() {
    wait_for_transfer();

    auto expected_win = cur_surf_info.bounds * (pixel_double ? 2 : 1);
    const uint16_t x = static_cast<uint16_t>((DISPLAY_WIDTH - expected_win.w) / 2);
    const uint16_t y = static_cast<uint16_t>((DISPLAY_HEIGHT - expected_win.h) / 2);

    // Always rewind the GRAM window. Leaving WRITE_RAM open across frames
    // lets a single extra/missing DCLK scroll the image by a row per flip —
    // the "lines travelling top to bottom" failure mode.
    set_window(x, y, static_cast<uint16_t>(expected_win.w), static_cast<uint16_t>(expected_win.h));
    prepare_write();

    if(pixel_double) {
        cur_scanline = 0;
        upd_frame_buffer = frame_buffer;
        dma_channel_set_trans_count(dma_channel, win_w / 4, false);
    } else {
        dma_channel_set_trans_count(dma_channel, static_cast<uint32_t>(win_w) * win_h, false);
    }

    dma_channel_set_read_addr(dma_channel, frame_buffer, true);
}

static void set_pixel_double(bool pd) {
    pixel_double = pd;

    if(write_mode)
        command(0);

    if(pixel_double) {
        dma_channel_acknowledge_irq0(dma_channel);
        dma_channel_set_irq0_enabled(dma_channel, true);
    } else
        dma_channel_set_irq0_enabled(dma_channel, false);
}

static void clear() {
    if(!write_mode)
        prepare_write();

    for(int i = 0; i < win_w * win_h; i++)
        pio_sm_put_blocking(pio, pio_sm, 0);

    pio_wait(pio, pio_sm);
}

#ifdef LCD_VSYNC_PIN
static void vsync_callback(uint gpio, uint32_t events) {
    (void)gpio;
    (void)events;
    if(!do_render && !dma_is_busy()) {
        ::update();
        do_render = true;
    }
}
#endif

void ssd1351_set_master_contrast(uint8_t level) {
    if(level > 0x0F)
        level = 0x0F;
    wait_for_transfer();
    const char v = static_cast<char>(level);
    command(SSD1351::CONTRAST_MASTER, 1, &v);
}

void init_display() {
    frame_buffer = screen_fb;

    gpio_set_function(LCD_DC_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(LCD_DC_PIN, GPIO_OUT);

    gpio_set_function(LCD_CS_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(LCD_CS_PIN, GPIO_OUT);
    gpio_put(LCD_CS_PIN, 1);

    bi_decl_if_func_used(bi_1pin_with_name(LCD_DC_PIN, "Display D/C"));
    bi_decl_if_func_used(bi_1pin_with_name(LCD_CS_PIN, "Display CS"));

#ifdef LCD_VSYNC_PIN
    gpio_set_function(LCD_VSYNC_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(LCD_VSYNC_PIN, GPIO_IN);
    gpio_set_pulls(LCD_VSYNC_PIN, false, true);
    bi_decl_if_func_used(bi_1pin_with_name(LCD_VSYNC_PIN, "Display TE/VSync"));
#endif

#ifdef DBI_8BIT
    gpio_init(LCD_RD_PIN);
    gpio_set_dir(LCD_RD_PIN, GPIO_OUT);
    gpio_put(LCD_RD_PIN, 1);

    bi_decl_if_func_used(bi_1pin_with_name(LCD_RD_PIN, "Display RD"));
#endif

#ifdef LCD_RESET_PIN
    gpio_set_function(LCD_RESET_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(LCD_RESET_PIN, GPIO_OUT);
    gpio_put(LCD_RESET_PIN, 0);
    sleep_ms(100);
    gpio_put(LCD_RESET_PIN, 1);

    bi_decl_if_func_used(bi_1pin_with_name(LCD_RESET_PIN, "Display Reset"));
#endif

    pio_offset = pio_add_program(pio, &dbi_raw_program);
    pio_double_offset = pio_add_program(pio, &dbi_pixel_double_program);

    pio_sm = pio_claim_unused_sm(pio, true);

    pio_sm_config cfg = dbi_raw_program_get_default_config(pio_offset);

#ifdef DBI_8BIT
    const int out_width = 8;
#else
    const int out_width = 1;
#endif

    // Stock HAL used ceil() so a 250 MHz sysclk / 20 MHz cap became clkdiv 7
    // → 17.86 MHz. Fractional divider hits the datasheet 20 MHz cap exactly.
    float clkdiv = static_cast<float>(clock_get_hz(clk_sys)) / static_cast<float>(LCD_MAX_CLOCK * 2);
    if(clkdiv < 1.0f)
        clkdiv = 1.0f;
    sm_config_set_clkdiv(&cfg, clkdiv);

    sm_config_set_out_shift(&cfg, false, true, 8);
    sm_config_set_out_pins(&cfg, LCD_MOSI_PIN, out_width);
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);
    sm_config_set_sideset_pins(&cfg, LCD_SCK_PIN);

    for(int i = 0; i < out_width; i++)
        pio_gpio_init(pio, LCD_MOSI_PIN + i);

    pio_gpio_init(pio, LCD_SCK_PIN);

    pio_sm_set_consecutive_pindirs(pio, pio_sm, LCD_MOSI_PIN, out_width, true);
    pio_sm_set_consecutive_pindirs(pio, pio_sm, LCD_SCK_PIN, 1, true);

    pio_sm_init(pio, pio_sm, pio_offset, &cfg);
    pio_sm_set_enabled(pio, pio_sm, true);

#ifdef DBI_8BIT
    bi_decl_if_func_used(bi_pin_mask_with_name(0xFF << LCD_MOSI_PIN, "Display Data"));
    bi_decl_if_func_used(bi_1pin_with_name(LCD_SCK_PIN, "Display WR"));
#else
    bi_decl_if_func_used(bi_1pin_with_name(LCD_MOSI_PIN, "Display TX"));
    bi_decl_if_func_used(bi_1pin_with_name(LCD_SCK_PIN, "Display SCK"));
#endif

    send_init_sequence();

    dma_channel = dma_claim_unused_channel(true);
    dma_channel_config config = dma_channel_get_default_config(dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_16);
    channel_config_set_read_increment(&config, true);
    channel_config_set_write_increment(&config, false);
    channel_config_set_dreq(&config, pio_get_dreq(pio, pio_sm, true));
    dma_channel_configure(
        dma_channel, &config, &pio->txf[pio_sm], frame_buffer, DISPLAY_WIDTH * DISPLAY_HEIGHT, false);
    dma_ready = true;

    irq_add_shared_handler(DMA_IRQ_0, dbi_dma_irq_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);

    clear();

#ifdef LCD_VSYNC_PIN
    gpio_set_irq_enabled_with_callback(LCD_VSYNC_PIN, GPIO_IRQ_EDGE_RISE, true, vsync_callback);
    have_vsync = true;
#endif
}

void update_display(uint32_t time) {
    if((do_render || (!have_vsync && time - last_render >= 20)) && (fb_double_buffer || !dma_is_busy())) {
        if(fb_double_buffer) {
            buf_index ^= 1;

            screen.data = (uint8_t *)screen_fb + (buf_index) * get_display_page_size();
            frame_buffer = (uint16_t *)screen.data;
        }

        ::render(time);

        if(!have_vsync) {
            while(dma_is_busy()) {}
            ::update();
        }

        last_render = time;
        do_render = false;
    }
}

void init_display_core1() {
}

void update_display_core1() {
}

bool display_render_needed() {
    return do_render;
}

bool display_mode_supported(blit::ScreenMode new_mode, const blit::SurfaceTemplate &new_surf_template) {
    (void)new_mode;
    if(new_surf_template.format != blit::PixelFormat::RGB565)
        return false;

    blit::Size expected_bounds(DISPLAY_WIDTH, DISPLAY_HEIGHT);

    if(new_surf_template.bounds.w <= expected_bounds.w && new_surf_template.bounds.h <= expected_bounds.h)
        return true;

    return false;
}

void display_mode_changed(blit::ScreenMode new_mode, blit::SurfaceTemplate &new_surf_template) {
    (void)new_surf_template;
    if(have_vsync)
        do_render = true;

    set_pixel_double(new_mode == ScreenMode::lores);

    if(new_mode == ScreenMode::hires)
        frame_buffer = screen_fb;
}

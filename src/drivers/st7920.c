#include "drivers/st7920.h"

#include <string.h>

#include "dgx_arch.h"
#include "dgx_bits.h"
#include "dgx_screen_functions.h"
#include "drivers/dgx_lcd_init.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/vscreen.h"

/**
 * @brief chip information definition
 */

#define CHIP_NAME          "Sitronix ST7920" /**< chip name */
#define MANUFACTURER_NAME  "Sitronix"        /**< manufacturer name */
#define SUPPLY_VOLTAGE_MIN 2.7f              /**< chip min supply voltage */
#define SUPPLY_VOLTAGE_MAX 4.5f              /**< chip max supply voltage */
#define MAX_CURRENT        0.45f             /**< chip max current */
#define TEMPERATURE_MIN    -30.0f            /**< chip min operating temperature */
#define TEMPERATURE_MAX    85.0f             /**< chip max operating temperature */
#define DRIVER_VERSION     1000              /**< driver version */

// ST7920 Commands

/**
 * @brief command definition
 */
#define ST7920_CMD_BASIC_DISPLAY_CLEAR                 0x01 /**< display clear command */
#define ST7920_CMD_BASIC_RETURN_HOME                   0x02 /**< return home command */
#define ST7920_CMD_BASIC_ENTRY_MODE_SET                0x04 /**< entry mode set command */
#define ST7920_CMD_BASIC_ENTRY_MODE_SET_LR             0x06 /**< entry mode set command */
#define ST7920_CMD_BASIC_DISPLAY_CONTROL               0x08 /**< display control command */
#define ST7920_CMD_BASIC_DISPLAY_CONTROL_ON_NORM       0x0C /**< display control command */
#define ST7920_CMD_BASIC_CURSOR_DISPLAY_CONTROL        0x10 /**< cursor display control command */
#define ST7920_CMD_BASIC_FUNCTION_SET                  0x20 /**< function set command */
#define ST7920_CMD_BASIC_FUNCTION_8BIT                 0x30 /**< function set command */
#define ST7920_CMD_BASIC_EXT_FUNCTION_SET              0x24 /**< function set command */
#define ST7920_CMD_BASIC_SET_CGRAM                     0x40 /**< set cgram command */
#define ST7920_CMD_BASIC_SET_DDRAM                     0x80 /**< set ddram command */
#define ST7920_CMD_EXT_STANFBY                         0x01 /**< standby command */
#define ST7920_CMD_EXT_SCROLL_RAM_ADDRESS_SELECT       0x02 /**< scroll or ram address select command */
#define ST7920_CMD_EXT_REVERSE                         0x04 /**< reverse command */
#define ST7920_CMD_EXT_FUNCTION_SET                    0x20 /**< extended function command */
#define ST7920_CMD_EXT_FUNCTION_8BIT                   0x34 /**< extended function command */
#define ST7920_CMD_EXT_GRAPHICS_ON                     0x36 /**< extended function command */
#define ST7920_CMD_EXT_SET_SCROLL_ADDRESS              0x40 /**< set scroll address command */
#define ST7920_CMD_EXT_SET_GRAPHIC_DISPLAY_RAM_ADDRESS 0x80 /**< set graphic display ram address command */

#define ST7920_COMMAND_DATA_DELAY_US 10

/*
Graphic RAM(GDRAM)
Graphic display RAM supports 64x256 bits bit-mapped memory space. GDRAM address is set by writing 2
consecutive bytes for vertical address and horizontal address. Two-bytes data write to GDRAM for one address.
Address counter will automatically increase by one for the next two-byte data. The procedure is as followings.
    1. Set vertical address(Y)for GDRAM
    2. Set horizontal address(X)for GDRAM
    3. Write D15〜D8 to GDRAM 中(first byte)
    4. Write D7〜D0 to GDRAM 中(second byte)
*/

#define LCD_LINE0 0x80
#define LCD_LINE1 0x90
#define LCD_LINE2 0x88
#define LCD_LINE3 0x98

#define ST7920_INTERFACE_BUS_BIT_4 0x00 /**< 4 bit */
#define ST7920_INTERFACE_BUS_BIT_8 0x01 /**< 8 bit */

// ST7920 display is 128x64, GDRAM is organized as 64 rows x 8 addresses (each address = 2 bytes = 16 pixels)
#define ST7920_WIDTH              128
#define ST7920_HEIGHT             64
#define ST7920_BYTES_PER_ROW      (ST7920_WIDTH / 8)
#define ST7920_ADDRESSES_PER_ROW  8  // 128 pixels / 16 pixels per address = 8 addresses
#define ST7920_PIXELS_PER_ADDRESS 16 // Each address = 2 bytes = 16 pixels

typedef struct _dgx_st7920_t
{
    dgx_vscreen_t        base;
    dgx_bus_protocols_t *bus;
    gpio_num_t           rst;
    gpio_num_t           cs;
} dgx_st7920_t;

static const char TAG[] = "DGX ST7920";

static const struct
{
    uint8_t cmd;
    int32_t delay; // in microseconds
} st_init_cmds[] = {
    //
    {ST7920_CMD_BASIC_FUNCTION_8BIT,           150 }, //
    {ST7920_CMD_BASIC_FUNCTION_8BIT,           150 }, //
    {ST7920_CMD_BASIC_DISPLAY_CLEAR,           150 }, //
    {ST7920_CMD_BASIC_ENTRY_MODE_SET_LR,       2000}, //
    {ST7920_CMD_BASIC_DISPLAY_CONTROL_ON_NORM, 2000}, //
    {ST7920_CMD_EXT_FUNCTION_8BIT,             2000}, //
    {ST7920_CMD_EXT_GRAPHICS_ON,               2000}, //
    {0,                                        -1  }  // Stop
};

// END OF COMMAND LIST

static void dgx_st7920_send_byte(dgx_st7920_t *scr, uint8_t data, uint8_t rs)
{
    uint8_t command[3];
    command[0] = 0xf8 | (rs ? 0x02 : 0x00); // RS = 1 for data, 0 for command
    command[1] = data & 0xf0;               // upper nibble
    command[2] = (data << 4) & 0xf0;        // lower nibble
    dgx_gpio_set_level(scr->cs, 1);
    scr->bus->write_data(scr->bus, command, 3 * 8);
    dgx_gpio_set_level(scr->cs, 0);
}

static void dgx_st7920_send_command(dgx_st7920_t *scr, uint8_t cmd)
{
    dgx_st7920_send_byte(scr, cmd, 0);
}

// Set GDRAM address (vertical Y: 0-63, horizontal X: 0-15, each address holds 2 bytes = ST7920_PIXELS_PER_ADDRESS
// pixels) Note: Address setting uses RS=0 (command mode), but data writes use RS=1 (data mode)
static void dgx_st7920_set_gdram_address(dgx_st7920_t *scr, uint8_t y, uint8_t x)
{
    if (y >= 32) {
        y -= 32;
        x += 8;
    }
    dgx_st7920_send_command(scr, ST7920_CMD_EXT_SET_GRAPHIC_DISPLAY_RAM_ADDRESS | (y & 0x3F));
    dgx_st7920_send_command(scr, ST7920_CMD_EXT_SET_GRAPHIC_DISPLAY_RAM_ADDRESS | (x & 0x0f));
}

static void dgx_st7920_destroy(dgx_screen_t **_pscr)
{
    if (*_pscr) {
        dgx_st7920_t *scr = (dgx_st7920_t *)(*_pscr);
        // Free the virtual screen buffer (same as vscreen destroy does)
        if (scr->base.v_array) {
            free(scr->base.v_array);
            scr->base.v_array = NULL;
        }
        free(*_pscr);
        *_pscr = NULL;
    }
}
/*

You are correct that the official ST7920 datasheet is notoriously vague and confusing regarding the exact GDRAM memory
mapping for 128x64 mode. The documentation mentions a graphic display area of 64x256 dots and provides a table (Table 7
in some versions) that outlines the general address ranges, but it doesn't clearly explain the non-linear structure for
128x64 displays.

The behavior you observed is a well-known "undocumented feature" or eccentricity of the controller that
has been discovered and verified by the embedded systems community and library developers through experimentation:

* Community Consensus: Discussions on forums like Arduino and EmbeddedRelated show engineers and hobbyists independently
confirming this exact mapping behavior after struggling with the official documentation. Library authors, such as those
for the popular U8g2 library, have fixed their drivers specifically to handle this non-linear mapping.

* Split Architecture: The physical reality of the 128x64 display is two 128x32 segments. The controller seems to handle
these as two distinct memory banks in its addressing scheme.

* Empirical Evidence: The fact that your code only works with the y >= 32 check and coordinate shift is strong empirical
evidence that this specific logic is required to align with the controller's internal addressing scheme, despite the
lack of clear documentation.

In summary, the reason you didn't find it in the official documentation is that the documentation is insufficient in
this area. The required logic is a practical necessity derived from reverse engineering the chip's actual behavior.


How does it handle 256x64 then?

The ST7920 controller does not inherently drive a 256x64 resolution display on its own. The controller chip itself is
designed with 32 common drivers and 64 segment drivers.

To achieve a 256x64 resolution, an external extension segment driver (ST7921) must be used in conjunction with the
ST7920. The ST7920 acts as the main controller/common driver, while the ST7921 provides the additional segment
(horizontal) drivers needed for the wider screen.

Memory Handling for 256x64

When the ST7920 is configured to work with the extension chip for a 256x64 display, the memory mapping becomes more
linear and utilizes the full 64x256 dot graphic display area (GDRAM) that the datasheet mentions.

The memory is accessed in the following logical structure:

* Vertical Address (Y): This directly corresponds to the display lines from 0 to 63. There is no y >= 32 check needed,
as the entire Y range maps continuously.

* Horizontal Address (X): The X address is still managed in 16-bit blocks (2 bytes per address). For a 256-pixel width,
you have 16 horizontal addresses (0 to 15), since 16 blocks * 16 pixels = 256 pixels.

In a 256x64 setup, the odd addressing behavior of the 128x64 display (where the X address jumps for the lower half) is
eliminated. The memory is treated as one continuous 64-line buffer, where each line holds 16 words (16-bit data chunks).

*/

// Update physical display from virtual buffer
static void dgx_st7920_update_screen(dgx_screen_t *_scr, int left, int right, int top, int bottom)
{
    dgx_st7920_t  *scr     = (dgx_st7920_t *)_scr;
    dgx_vscreen_t *vscreen = &scr->base;

    ESP_LOGI(TAG, "Updating screen: left=%d, right=%d, top=%d, bottom=%d", left, right, top, bottom);

    // Clamp coordinates to display bounds
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= ST7920_WIDTH) right = ST7920_WIDTH - 1;
    if (bottom >= ST7920_HEIGHT) bottom = ST7920_HEIGHT - 1;

    if (left > right || top > bottom) return;

    int     addr_start = left / ST7920_PIXELS_PER_ADDRESS;
    int     addr_end   = right / ST7920_PIXELS_PER_ADDRESS;
    uint8_t row_bytes[ST7920_BYTES_PER_ROW * 2 + 1];
    for (int y = top; y <= bottom; y++) {
        // Set GDRAM address for the start of this row
        dgx_st7920_set_gdram_address(scr, y, addr_start);
        int idx_buf  = 1;
        row_bytes[0] = 0xfa; // Data write command
        for (int addr_x = addr_start; addr_x <= addr_end; addr_x++) {
            uint32_t byte_offset = (addr_x * 2) + (y * ST7920_BYTES_PER_ROW);
            uint8_t *lp          = vscreen->v_array + byte_offset;
            row_bytes[idx_buf++] = lp[0] & 0xf0;        //
            row_bytes[idx_buf++] = (lp[0] << 4) & 0xf0; //
            row_bytes[idx_buf++] = lp[1] & 0xf0;        //
            row_bytes[idx_buf++] = (lp[1] << 4) & 0xf0; //
        }
        // Send the row data
        dgx_gpio_set_level(scr->cs, 1);
        scr->bus->write_data(scr->bus, row_bytes, idx_buf * 8);
        dgx_gpio_set_level(scr->cs, 0);
    }
}

// Initialize the display
dgx_screen_t *dgx_st7920_init(dgx_bus_protocols_t *bus, gpio_num_t rst, gpio_num_t cs)
{
    dgx_st7920_t *scr = (dgx_st7920_t *)calloc(1, sizeof(dgx_st7920_t));
    if (!scr) {
        ESP_LOGE(TAG, "Screen structure memory allocation failed");
        return NULL;
    }

    scr->cs  = cs;
    scr->rst = rst;
    scr->bus = bus;

    // Initialize virtual screen (128x64, 1 bit per pixel, monochrome)
    dgx_screen_t *vscreen = dgx_vscreen_init(ST7920_WIDTH, ST7920_HEIGHT, 1, DgxScreenRGB);
    if (!vscreen) {
        ESP_LOGE(TAG, "Virtual screen initialization failed");
        free(scr);
        return NULL;
    }

    // Copy the vscreen structure
    scr->base = *(dgx_vscreen_t *)vscreen;
    free(vscreen);
    scr->base.base.screen_subtype = DgxVirtualBackScreen;
    scr->base.base.screen_name    = "ST7920";
    // Setup GPIO
    dgx_gpio_set_direction(cs, GPIO_MODE_OUTPUT);
    dgx_gpio_set_level(cs, 0);

    // Reset the display
    if (rst >= 0) {
        dgx_delay(150); // wait for power to stabilize
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        dgx_gpio_set_level(rst, 0);
        dgx_delay(100);
        dgx_gpio_set_level(rst, 1);
        dgx_delay(100);
    }

    // Send initialization commands
    for (size_t cmd_idx = 0; st_init_cmds[cmd_idx].delay != -1; cmd_idx++) {
        dgx_delay_us(st_init_cmds[cmd_idx].delay);
        ESP_LOGD(TAG, "Sending init command: 0x%02x", st_init_cmds[cmd_idx].cmd);
        dgx_st7920_send_command(scr, st_init_cmds[cmd_idx].cmd);
    }
    dgx_delay(2);

    // Wire up function pointers - only override what's needed
    // vscreen already provides: set_area, write_area, read_area, set_pixel, get_pixel,
    //                            fill_rectangle, draw_line, circle, solid_circle, wait_buffer
    scr->base.base.update_screen = dgx_st7920_update_screen;
    scr->base.base.destroy       = dgx_st7920_destroy;
    // Enable debug logging
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    return (dgx_screen_t *)scr;
}

void dgx_st7920_orientation(dgx_screen_t *scr_, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    // dgx_st7920_t *scr = (dgx_st7920_t *)scr_;
    // dgx_vscreen_orientation(scr->base, dir_x, dir_y, swap_xy);
    (void)scr_;
    (void)dir_x;
    (void)dir_y;
    (void)swap_xy;
}

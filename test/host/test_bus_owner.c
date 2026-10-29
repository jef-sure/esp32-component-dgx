/*
 * Who owns the bus under a display: the I2C and the 8-bit parallel buses,
 * src/bus/i2c_esp32.c and src/bus/p8_esp32.c, against drivers of IDF that
 * count what is made and deleted.
 *
 * A display made by dgx_i2c_init() or dgx_p8_init() is on a bus DGX makes.
 * Two displays on one I2C port share it, and it is deleted with the last of
 * them, unless the program has put a device of its own on it. An I80 bus
 * takes one display and is deleted with it: the driver of IDF does not keep
 * two panels on one bus apart. A display made by ..._init_on_bus() is on the
 * program's bus, which DGX never deletes.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

/* both sources name their log tag TAG */
#define TAG TAG_I2C
#include "bus/i2c_esp32.c"
#undef TAG
#define TAG TAG_P8
#include "bus/p8_esp32.c"
#undef TAG

/* ---- the I2C driver ---- */

typedef struct {
    int  devices;
    bool deleted;
} fake_i2c_bus_t;

static int  i2c_buses_made, i2c_buses_deleted, i2c_devices;
static bool i2c_answer = true;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *bus_config, i2c_master_bus_handle_t *ret_bus_handle)
{
    (void)bus_config;
    *ret_bus_handle = calloc(1, sizeof(fake_i2c_bus_t));
    ++i2c_buses_made;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus_handle)
{
    fake_i2c_bus_t *b = bus_handle;
    if (b->devices) return ESP_ERR_INVALID_STATE; /* as IDF: devices still on it */
    free(b);
    ++i2c_buses_deleted;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int xfer_timeout_ms)
{
    (void)bus_handle, (void)address, (void)xfer_timeout_ms;
    return i2c_answer ? ESP_OK : ESP_ERR_NOT_FOUND;
}

typedef struct {
    fake_i2c_bus_t *bus;
} fake_i2c_dev_t;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle, const i2c_device_config_t *dev_config, i2c_master_dev_handle_t *ret_handle)
{
    (void)dev_config;
    fake_i2c_dev_t *d = calloc(1, sizeof(*d));
    d->bus            = bus_handle;
    ++d->bus->devices;
    ++i2c_devices;
    *ret_handle = d;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle)
{
    fake_i2c_dev_t *d = handle;
    --d->bus->devices;
    --i2c_devices;
    free(d);
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t i2c_dev, const uint8_t *write_buffer, size_t write_size, int xfer_timeout_ms)
{
    (void)i2c_dev, (void)write_buffer, (void)write_size, (void)xfer_timeout_ms;
    return ESP_OK;
}

/* ---- the I80 driver ---- */

struct esp_lcd_i80_bus_t {
    int ios;
};
struct esp_lcd_panel_io_t {
    esp_lcd_i80_bus_handle_t bus;
};

static int i80_buses_made, i80_buses_deleted;

esp_err_t esp_lcd_new_i80_bus(const esp_lcd_i80_bus_config_t *bus_config, esp_lcd_i80_bus_handle_t *ret_bus)
{
    (void)bus_config;
    *ret_bus = calloc(1, sizeof(**ret_bus));
    ++i80_buses_made;
    return ESP_OK;
}

esp_err_t esp_lcd_del_i80_bus(esp_lcd_i80_bus_handle_t bus)
{
    if (bus->ios) return ESP_ERR_INVALID_STATE;
    free(bus);
    ++i80_buses_deleted;
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_io_i80(esp_lcd_i80_bus_handle_t bus, const esp_lcd_panel_io_i80_config_t *io_config, esp_lcd_panel_io_handle_t *ret_io)
{
    (void)io_config;
    *ret_io        = calloc(1, sizeof(**ret_io));
    (*ret_io)->bus = bus;
    ++bus->ios;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io)
{
    --io->bus->ios;
    free(io);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
    (void)io, (void)lcd_cmd, (void)param, (void)param_size;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *color, size_t color_size)
{
    (void)io, (void)lcd_cmd, (void)color, (void)color_size;
    return ESP_OK;
}

/* ---- the checks ---- */

static void test_i2c(void)
{
    /* two displays of DGX on one port: one master bus, deleted with the second */
    dgx_bus_protocols_t *a = dgx_i2c_init(I2C_NUM_0, 0x3c, 21, 22, 400000);
    dgx_bus_protocols_t *b = dgx_i2c_init(I2C_NUM_0, 0x3d, 21, 22, 400000);
    CHECK(a && b && i2c_buses_made == 1 && i2c_devices == 2);
    CHECK(((dgx_i2c_bus_t *)a)->bus_handle == ((dgx_i2c_bus_t *)b)->bus_handle);
    /* other pins on the same port are refused, and nothing is left behind */
    CHECK(dgx_i2c_init(I2C_NUM_0, 0x3e, 18, 19, 400000) == NULL && i2c_devices == 2);
    a->dispose(a);
    CHECK(i2c_buses_deleted == 0);
    b->dispose(b);
    CHECK(i2c_buses_deleted == 1 && i2c_devices == 0);

    /* a device of the program on DGX's bus keeps it when the display goes; the next display takes the same bus */
    a = dgx_i2c_init(I2C_NUM_1, 0x3c, 21, 22, 400000);
    i2c_master_dev_handle_t touch;
    i2c_device_config_t     cfg = {.device_address = 0x38};
    i2c_master_bus_add_device(((dgx_i2c_bus_t *)a)->bus_handle, &cfg, &touch);
    i2c_master_bus_handle_t kept = ((dgx_i2c_bus_t *)a)->bus_handle;
    a->dispose(a);
    CHECK(i2c_buses_deleted == 1);
    a = dgx_i2c_init(I2C_NUM_1, 0x3c, 21, 22, 400000);
    CHECK(a && ((dgx_i2c_bus_t *)a)->bus_handle == kept && i2c_buses_made == 2);
    i2c_master_bus_rm_device(touch);
    a->dispose(a);
    CHECK(i2c_buses_deleted == 2);

    /* a display that does not answer: no device, and the bus made for it is deleted again */
    i2c_answer = false;
    CHECK(dgx_i2c_init(I2C_NUM_0, 0x3c, 21, 22, 400000) == NULL);
    CHECK(i2c_devices == 0 && i2c_buses_made == 3 && i2c_buses_deleted == 3);
    i2c_answer = true;

    /* the program's bus: DGX adds and removes its device and never deletes the bus */
    i2c_master_bus_handle_t own;
    i2c_master_bus_config_t own_cfg = {.i2c_port = I2C_NUM_0};
    i2c_new_master_bus(&own_cfg, &own);
    a = dgx_i2c_init_on_bus(own, 0x3c, 400000);
    b = dgx_i2c_init_on_bus(own, 0x3d, 400000);
    CHECK(a && b && ((fake_i2c_bus_t *)own)->devices == 2);
    a->dispose(a);
    b->dispose(b);
    CHECK(((fake_i2c_bus_t *)own)->devices == 0 && i2c_buses_deleted == 3);
    CHECK(i2c_del_master_bus(own) == ESP_OK);
    CHECK(dgx_i2c_init_on_bus(NULL, 0x3c, 400000) == NULL);
}

static void test_p8(void)
{
    /* a display on a bus DGX creates for it */
    dgx_bus_protocols_t *a = dgx_p8_init(1, 2, 3, 4, 5, 6, 7, 8, 9, GPIO_NUM_NC, 10, 11, 10000000);
    CHECK(a && i80_buses_made == 1);
    /* a second display on the same pins with a CS of its own is refused, as is a pin of that bus used otherwise */
    CHECK(dgx_p8_init(1, 2, 3, 4, 5, 6, 7, 8, 9, GPIO_NUM_NC, 12, 11, 10000000) == NULL);
    CHECK(dgx_p8_init(1, 2, 3, 4, 5, 6, 7, 8, 13, GPIO_NUM_NC, 14, 11, 10000000) == NULL && i80_buses_made == 1);
    /* on pins of its own it gets a bus of its own */
    dgx_bus_protocols_t *b = dgx_p8_init(21, 22, 23, 24, 25, 26, 27, 28, 29, GPIO_NUM_NC, 30, 31, 10000000);
    CHECK(b && i80_buses_made == 2 && ((dgx_p8_bus_t *)a)->i80_bus != ((dgx_p8_bus_t *)b)->i80_bus);
    /* each bus is deleted with its display, and its pins are free again */
    a->dispose(a);
    CHECK(i80_buses_deleted == 1);
    b->dispose(b);
    CHECK(i80_buses_deleted == 2);
    a = dgx_p8_init(1, 2, 3, 4, 5, 6, 7, 8, 9, GPIO_NUM_NC, 12, 11, 10000000);
    CHECK(a && i80_buses_made == 3);
    a->dispose(a);
    CHECK(i80_buses_deleted == 3);

    /* the program's bus is never deleted by DGX */
    esp_lcd_i80_bus_handle_t own;
    esp_lcd_i80_bus_config_t cfg = {.bus_width = 8, .max_transfer_bytes = 4096};
    esp_lcd_new_i80_bus(&cfg, &own);
    a = dgx_p8_init_on_bus(own, GPIO_NUM_NC, 10, 10000000);
    CHECK(a && own->ios == 1);
    a->dispose(a);
    CHECK(own->ios == 0 && i80_buses_deleted == 3);
    CHECK(esp_lcd_del_i80_bus(own) == ESP_OK);
}

int main(void)
{
    test_i2c();
    test_p8();
    CHECK_DONE();
}

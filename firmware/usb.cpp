// SPDX-License-Identifier: Apache-2.0
#include "usb.hpp"
#include "tusb.h"
#include "pico/unique_id.h"
#include <algorithm>
#include <cstring>

namespace {
const tusb_desc_device_t device = {
    sizeof(tusb_desc_device_t), TUSB_DESC_DEVICE, 0x0200, TUSB_CLASS_MISC,
    MISC_SUBCLASS_COMMON, MISC_PROTOCOL_IAD, 64, 0xcafe, 0x4011,
    0x0100, 1, 2, 3, 1,
};
const uint8_t config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 0, 0x81, 8, 0x02, 0x82, 64),
};
char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
char input[40];
size_t used = 0;
bool discard = false;
}
extern "C" const uint8_t* tud_descriptor_device_cb(void) {
    return reinterpret_cast<const uint8_t*>(&device);
}
extern "C" const uint8_t* tud_descriptor_configuration_cb(uint8_t) { return config; }
extern "C" const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t) {
    static uint16_t out[32];
    if (index == 0) { out[0] = 0x0304; out[1] = 0x0409; return out; }
    const char* const labels[] = {"", "Rivercraft", "Ephemeris 1", serial};
    if (index >= sizeof(labels) / sizeof(labels[0])) return nullptr;
    size_t n = std::min(std::strlen(labels[index]), size_t(31));
    out[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    for (size_t i = 0; i < n; ++i) out[i + 1] = uint8_t(labels[index][i]);
    return out;
}
namespace usb {
void init() {
    pico_get_unique_board_id_string(serial, sizeof(serial));
    tud_init(0);
}
void poll() { tud_task_ext(0, false); }
bool connected() { return tud_cdc_connected(); }
size_t write(const void* data, size_t n) {
    if (!connected()) return 0;
    size_t len = std::min(n, size_t(tud_cdc_write_available()));
    if (!len) return 0;
    size_t sent = tud_cdc_write(data, len);
    tud_cdc_write_flush();
    return sent;
}
size_t read(void* buffer, size_t capacity) {
    return tud_cdc_read(buffer, std::min(capacity, size_t(tud_cdc_available())));
}
bool command(char* buffer, size_t capacity) {
    if (!connected()) { used = 0; discard = false; return false; }
    // Bound work per event, even if the host continuously sends garbage.
    for (unsigned i = 0; i < 32 && tud_cdc_available(); ++i) {
        int c = tud_cdc_read_char();
        if (c == '\r') continue;
        if (c == '\n') {
            bool good = !discard && used && used < capacity;
            if (good) { std::memcpy(buffer, input, used); buffer[used] = 0; }
            used = 0; discard = false;
            if (good) return true;
        } else if (c >= 32 && c <= 126 && used < sizeof(input)) {
            input[used++] = char(c);
        } else {
            discard = true;
        }
    }
    return false;
}
}

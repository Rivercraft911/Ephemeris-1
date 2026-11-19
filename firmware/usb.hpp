// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
namespace usb {
void init();
void poll();
bool connected();
// Once connected, return the number accepted immediately, without waiting for a host.
size_t write(const void* data, size_t n);
size_t read(void* buffer, size_t capacity);
bool command(char* buffer, size_t capacity);
}

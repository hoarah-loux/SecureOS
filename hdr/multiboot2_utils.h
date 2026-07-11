#pragma once

#include <stdint.h>
#include <multiboot2.h>

const struct multiboot_tag *mb2_find_tag(uint32_t mb2_info_ptr, uint32_t want_type);

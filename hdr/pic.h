#pragma once

#include <stdint.h>

#define PIC1_VECTOR_BASE 0x20u
#define PIC2_VECTOR_BASE 0x28u
#define PIC_KEYBOARD_VECTOR (PIC1_VECTOR_BASE + 1u)

void pic_remap(uint8_t master_offset, uint8_t slave_offset);
void pic_set_mask(uint8_t irq_line);
void pic_clear_mask(uint8_t irq_line);
void pic_send_eoi(uint8_t irq_line);
void pic_init_for_keyboard(void);

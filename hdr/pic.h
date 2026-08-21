#pragma once

#include <stdint.h>

#define PIC1_VECTOR_BASE 0x20u
#define PIC2_VECTOR_BASE 0x28u
#define PIC_TIMER_IRQ 0u
#define PIC_KEYBOARD_IRQ 1u
#define PIC_MOUSE_IRQ 12u
#define PIC_TIMER_VECTOR (PIC1_VECTOR_BASE + PIC_TIMER_IRQ)
#define PIC_KEYBOARD_VECTOR (PIC1_VECTOR_BASE + PIC_KEYBOARD_IRQ)
#define PIC_MOUSE_VECTOR (PIC2_VECTOR_BASE + (PIC_MOUSE_IRQ - 8u))

void pic_remap(uint8_t master_offset, uint8_t slave_offset);
void pic_set_mask(uint8_t irq_line);
void pic_clear_mask(uint8_t irq_line);
void pic_send_eoi(uint8_t irq_line);
void pic_init_for_keyboard(void);
void pic_enable_mouse_irq(void);

#include <pic.h>

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

enum {
    PIC1_COMMAND = 0x20,
    PIC1_DATA = 0x21,
    PIC2_COMMAND = 0xA0,
    PIC2_DATA = 0xA1,
    PIC_EOI = 0x20,
    ICW1_ICW4 = 0x01,
    ICW1_INIT = 0x10,
    ICW4_8086 = 0x01
};

void pic_send_eoi(uint8_t irq_line) {
    if (irq_line >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_remap(uint8_t master_offset, uint8_t slave_offset) {
    uint8_t master_mask = inb(PIC1_DATA);
    uint8_t slave_mask = inb(PIC2_DATA);

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, master_offset);
    io_wait();
    outb(PIC2_DATA, slave_offset);
    io_wait();

    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    outb(PIC1_DATA, master_mask);
    outb(PIC2_DATA, slave_mask);
}

void pic_set_mask(uint8_t irq_line) {
    uint16_t data_port;
    uint8_t value;

    if (irq_line < 8) {
        data_port = PIC1_DATA;
    } else {
        data_port = PIC2_DATA;
        irq_line = (uint8_t)(irq_line - 8);
    }

    value = inb(data_port);
    value = (uint8_t)(value | (uint8_t)(1u << irq_line));
    outb(data_port, value);
}

void pic_clear_mask(uint8_t irq_line) {
    uint16_t data_port;
    uint8_t value;

    if (irq_line < 8) {
        data_port = PIC1_DATA;
    } else {
        data_port = PIC2_DATA;
        irq_line = (uint8_t)(irq_line - 8);
    }

    value = inb(data_port);
    value = (uint8_t)(value & (uint8_t)~(1u << irq_line));
    outb(data_port, value);
}

void pic_init_for_keyboard(void) {
    pic_remap(PIC1_VECTOR_BASE, PIC2_VECTOR_BASE);

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    // Keep timer masked until full runtime validation.
    pic_set_mask(PIC_TIMER_IRQ);

    // Enable keyboard IRQ1.
    pic_clear_mask(PIC_KEYBOARD_IRQ);
}

void pic_enable_mouse_irq(void) {
    // Enable cascade on master IRQ2 so slave IRQs can be forwarded.
    pic_clear_mask(2u);

    // Enable mouse IRQ12 on the slave PIC.
    pic_clear_mask(PIC_MOUSE_IRQ);
}

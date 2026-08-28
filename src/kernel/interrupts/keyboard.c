#include <keyboard.h>
#include <p.h>
#include <scheduler.h>
#include <t.h>

enum {
    KEYBOARD_BUFFER_SIZE = 256,
    SCANCODE_BUFFER_SIZE = 256
};

static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static uint32_t keyboard_head = 0;
static uint32_t keyboard_tail = 0;
static uint8_t keyboard_shift_down = 0;

// The raw event ring, fed by the same interrupt as the cooked one. Separate
// storage rather than one ring read two ways: cooked characters depend on
// modifier state at the moment the key was pressed, which cannot be
// reconstructed later from a scancode alone.
static uint16_t scancode_buffer[SCANCODE_BUFFER_SIZE];
static uint32_t scancode_head = 0;
static uint32_t scancode_tail = 0;

// Set by an 0xE0 byte, consumed by the byte after it. The prefix is not an
// event of its own and is never enqueued.
static uint8_t scancode_extended_pending = 0;

static int kbd_raw_mode_on = 0;

static const char keymap_base[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' '
};

static const char keymap_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}',
    [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x39] = ' '
};

static void scancode_enqueue(uint16_t event) {
    uint32_t next_head = (scancode_head + 1u) % SCANCODE_BUFFER_SIZE;
    if (next_head == scancode_tail) {
        return;   // full: drop the newest, as the cooked ring does
    }

    scancode_buffer[scancode_head] = event;
    scancode_head = next_head;
}

// Throw away everything buffered, in both rings.
//
// Called whenever input ownership moves. Without it the next foreground
// process inherits whatever the previous one did not read - a game's movement
// keys arriving at a shell prompt as a line of text, or the other way round.
static void keyboard_flush(void) {
    keyboard_tail = keyboard_head;
    scancode_tail = scancode_head;
    scancode_extended_pending = 0;
    keyboard_shift_down = 0;
}

static void keyboard_enqueue(char c) {
    uint32_t next_head = (keyboard_head + 1u) % KEYBOARD_BUFFER_SIZE;
    if (next_head == keyboard_tail) {
        return;
    }

    keyboard_buffer[keyboard_head] = c;
    keyboard_head = next_head;
}

int keyboard_handle_scancode(uint8_t scancode) {
    // 0xE0 introduces the key after it; it is a prefix, not an event.
    if (scancode == 0xE0u) {
        scancode_extended_pending = 1;
        return 0;
    }

    // 0xE1 introduces Pause/Break, which arrives as a six-byte sequence with
    // no release at all. Decoding it properly means a small state machine for
    // one key nothing here binds, so the prefix is dropped and its payload
    // bytes are decoded as if they were ordinary keys. Worth knowing about
    // rather than worth handling.
    if (scancode == 0xE1u) {
        return 0;
    }

    uint16_t extended = scancode_extended_pending ? KBD_SC_EXTENDED : 0;
    scancode_extended_pending = 0;

    uint16_t released = (scancode & 0x80u) ? KBD_SC_RELEASED : 0;
    uint16_t base = (uint16_t)(scancode & 0x7Fu);

    scancode_enqueue((uint16_t)(base | extended | released));

    // In raw mode the cooked ring is deliberately left alone - see the comment
    // on keyboard_set_raw_mode() in hdr/keyboard.h. The event above is still
    // worth a wake, so this returns 1 either way.
    if (kbd_raw_mode_on) {
        return 1;
    }

    // Shift, but only the real ones. PrintScreen and the extended arrow keys
    // emit 0xE0 0x2A / 0xE0 0xAA "fake shifts" for the benefit of DOS-era
    // software; treating those as a real shift press would make the next
    // character come out capitalised for no reason the user can see.
    if (extended == 0 && (base == 0x2Au || base == 0x36u)) {
        keyboard_shift_down = released ? 0 : 1;
        return 1;
    }

    // Everything below produces a character, and only a press produces one.
    if (released) {
        return 1;
    }

    // Extended keys - arrows, RCtrl, RAlt, keypad Enter - have no ASCII, which
    // is exactly why raw mode exists.
    if (extended) {
        return 1;
    }

    char c = keyboard_shift_down ? keymap_shift[base] : keymap_base[base];
    if (c != 0) {
        keyboard_enqueue(c);
    }

    return 1;
}

// ---------------- input ownership ----------------
//
// 0 means nobody, which is the state before the shell exists. A read by a
// process that owns nothing blocks, which is the safe direction: keystrokes
// wait in the ring buffer rather than being handed to the wrong reader.
static uint64_t kbd_owner_pid;
static uint64_t kbd_default_owner_pid;

void keyboard_set_default_owner(uint64_t pid) {
    kbd_default_owner_pid = pid;

    if (kbd_owner_pid == 0) {
        kbd_owner_pid = pid;
    }
}

void keyboard_grant_input(uint64_t pid) {
    // The incoming process starts with a clean slate in both rings, and in
    // cooked mode, whatever the outgoing one was doing.
    keyboard_flush();
    kbd_raw_mode_on = 0;
    kbd_owner_pid = pid;
}

void keyboard_release_input(uint64_t pid) {
    // Only the holder can lose it. A background process exiting must not pull
    // the keyboard out from under whatever is actually in the foreground.
    if (pid != 0 && pid == kbd_owner_pid) {
        // Same clean slate on the way back. A program that died in raw mode
        // must not leave the shell unable to read a character, and the keys
        // its player was holding must not arrive at a shell prompt.
        keyboard_flush();
        kbd_raw_mode_on = 0;
        kbd_owner_pid = kbd_default_owner_pid;
    }
}

uint64_t keyboard_input_owner(void) {
    return kbd_owner_pid;
}

// Whether the process on this CPU right now is the one allowed to consume a
// keystroke.
int keyboard_caller_owns_input(void) {
    scheduler_cpu *cpu = scheduler_this_cpu();
    if (cpu == NULL || cpu->current_thread == NULL) { return 0; }

    proc *p = cpu->current_thread->owning_proc;
    if (p == NULL) { return 0; }

    return p->id == kbd_owner_pid;
}

int keyboard_getchar_nonblock(char *out_char) {
    if (!out_char || keyboard_head == keyboard_tail) {
        return 0;
    }

    *out_char = keyboard_buffer[keyboard_tail];
    keyboard_tail = (keyboard_tail + 1u) % KEYBOARD_BUFFER_SIZE;
    return 1;
}

void keyboard_set_raw_mode(int enabled) {
    // Flush across the transition in both directions: characters typed under
    // the old mode were meant for the old interpretation of the keyboard.
    keyboard_flush();
    kbd_raw_mode_on = enabled ? 1 : 0;
}

int keyboard_raw_mode(void) {
    return kbd_raw_mode_on;
}

static int scancode_dequeue(uint16_t *out_event) {
    if (!out_event || scancode_head == scancode_tail) {
        return 0;
    }

    *out_event = scancode_buffer[scancode_tail];
    scancode_tail = (scancode_tail + 1u) % SCANCODE_BUFFER_SIZE;
    return 1;
}

int keyboard_getline(char *buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return -1;
    }

    size_t index = 0;
    while (index < buffer_size - 1) {
        char c;
        if (!keyboard_getchar_nonblock(&c)) {
            continue;
        }

        if (c == '\n' || c == '\r') {
            buffer[index] = '\0';
            return (int)index;
        }

        buffer[index++] = c;
    }

    buffer[index] = '\0';
    return (int)index;
}

int64_t sys_getkey(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    // A process that does not own input has no key waiting for it, however
    // full the buffer is. Reported as "nothing available" rather than as an
    // error, because that is what it means to the caller and it is the answer
    // a polling loop already knows how to handle.
    if (!keyboard_caller_owns_input()) {
        return -1;
    }

    char c;
    if (!keyboard_getchar_nonblock(&c)) {
        return -1;
    }
    return (int64_t)(unsigned char)c;
}

int64_t sys_getkey_wait(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    // Not owning input is a reason to wait, not to fail: the shell spends its
    // whole life here, and while a program is in the foreground the shell must
    // sit still rather than spin. It is re-checked on every wake, so ownership
    // returning is enough to let it through - see keyboard_release_input().
    if (!keyboard_caller_owns_input()) {
        return SYS_WOULD_BLOCK;
    }

    char c;
    if (!keyboard_getchar_nonblock(&c)) {
        return SYS_WOULD_BLOCK;
    }
    return (int64_t)(unsigned char)c;
}

int64_t sys_getscancode(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (!keyboard_caller_owns_input()) {
        return -1;
    }

    uint16_t event;
    if (!scancode_dequeue(&event)) {
        return -1;
    }

    // Widened to a positive int64_t, so -1 stays unambiguous: no event can
    // encode to it, since bits 9-14 are always zero.
    return (int64_t)event;
}

int64_t sys_kbd_raw(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    // A background process must not be able to switch the keyboard out from
    // under the foreground one, which in cooked mode would leave the shell
    // silently unable to read a character.
    if (!keyboard_caller_owns_input()) {
        return -1;
    }

    keyboard_set_raw_mode(a0 != 0);
    return 0;
}

// ---------------- i8042 controller bring-up ----------------
//
// The kernel used to touch the i8042 only from the IRQ1 handler and the
// timer-tick poll, never initializing it. Fine in QEMU; on the bare-metal
// target (firmware USB legacy emulation) the controller came up wedged:
// status 0x55 = a stale byte in the output buffer plus the timeout-error
// bit, and no scancodes ever flowed. This is the standard minimal init:
// drain the output buffer, enable port 1 with IRQ + set-1 translation in
// the config byte, and tell the device to start scanning. Every write
// waits for IBF to clear and every response read waits for OBF, all
// bounded, so a machine with no controller at all (status 0xFF, floating
// bus) falls straight through without hanging.

enum {
    I8042_DATA_PORT = 0x60,
    I8042_STATUS_PORT = 0x64,
    I8042_CMD_PORT = 0x64,
    I8042_STATUS_OBF = 0x01,
    I8042_STATUS_IBF = 0x02,
    I8042_SPINS = 65536,
    I8042_FLUSH_MAX = 32
};

static inline uint8_t i8042_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void i8042_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

// Wait until the controller will accept a command/data byte (IBF clear).
static int i8042_wait_write_ready(void) {
    for (uint32_t i = 0; i < I8042_SPINS; i++) {
        uint8_t status = i8042_inb(I8042_STATUS_PORT);
        if (status == 0xFF) {
            return 0;
        }
        if ((status & I8042_STATUS_IBF) == 0) {
            return 1;
        }
    }
    return 0;
}

// Wait until a response byte is available (OBF set).
static int i8042_wait_read_ready(void) {
    for (uint32_t i = 0; i < I8042_SPINS; i++) {
        uint8_t status = i8042_inb(I8042_STATUS_PORT);
        if (status == 0xFF) {
            return 0;
        }
        if ((status & I8042_STATUS_OBF) != 0) {
            return 1;
        }
    }
    return 0;
}

// Drain whatever sits in the output buffer - stale bytes, error markers,
// leftovers from the firmware. Bounded so a stuck OBF bit cannot hang boot.
static void i8042_flush(void) {
    for (uint32_t i = 0; i < I8042_FLUSH_MAX; i++) {
        uint8_t status = i8042_inb(I8042_STATUS_PORT);
        if (status == 0xFF || (status & I8042_STATUS_OBF) == 0) {
            return;
        }
        (void)i8042_inb(I8042_DATA_PORT);
    }
}

void i8042_init(void) {
    if (i8042_inb(I8042_STATUS_PORT) == 0xFF) {
        return; // floating bus: no controller, nothing to set up
    }

    i8042_flush();

    // Config byte: enable port 1 IRQ (bit 0) and set-1 translation (bit 6),
    // clear the port 1 clock-disable bit (bit 4). Read-modify-write so any
    // firmware bits we do not understand survive.
    if (i8042_wait_write_ready()) {
        i8042_outb(I8042_CMD_PORT, 0x20);
        if (i8042_wait_read_ready()) {
            uint8_t config = i8042_inb(I8042_DATA_PORT);
            config |= 0x01;
            config |= 0x40;
            config &= (uint8_t)~0x10;
            if (i8042_wait_write_ready()) {
                i8042_outb(I8042_CMD_PORT, 0x60);
                if (i8042_wait_write_ready()) {
                    i8042_outb(I8042_DATA_PORT, config);
                }
            }
        }
    }

    // Enable port 1 explicitly, then tell the device to start scanning
    // (0xF4). Eat the ACK (0xFA) if one comes; tolerate silence.
    if (i8042_wait_write_ready()) {
        i8042_outb(I8042_CMD_PORT, 0xAE);
    }
    i8042_flush();

    if (i8042_wait_write_ready()) {
        i8042_outb(I8042_DATA_PORT, 0xF4);
        if (i8042_wait_read_ready()) {
            (void)i8042_inb(I8042_DATA_PORT);
        }
    }

    i8042_flush();
}

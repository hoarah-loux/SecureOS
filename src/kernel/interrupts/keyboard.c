#include <keyboard.h>
#include <p.h>
#include <scheduler.h>
#include <t.h>

enum {
    KEYBOARD_BUFFER_SIZE = 256
};

static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static uint32_t keyboard_head = 0;
static uint32_t keyboard_tail = 0;
static uint8_t keyboard_shift_down = 0;

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

static void keyboard_enqueue(char c) {
    uint32_t next_head = (keyboard_head + 1u) % KEYBOARD_BUFFER_SIZE;
    if (next_head == keyboard_tail) {
        return;
    }

    keyboard_buffer[keyboard_head] = c;
    keyboard_head = next_head;
}

int keyboard_handle_scancode(uint8_t scancode) {
    if (scancode == 0x2A || scancode == 0x36) {
        keyboard_shift_down = 1;
        return 0;
    }

    if (scancode == 0xAA || scancode == 0xB6) {
        keyboard_shift_down = 0;
        return 0;
    }

    if (scancode & 0x80u) {
        return 0;
    }

    char c = keyboard_shift_down ? keymap_shift[scancode] : keymap_base[scancode];
    if (c != 0) {
        keyboard_enqueue(c);
        return 1;
    }

    return 0;
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
    kbd_owner_pid = pid;
}

void keyboard_release_input(uint64_t pid) {
    // Only the holder can lose it. A background process exiting must not pull
    // the keyboard out from under whatever is actually in the foreground.
    if (pid != 0 && pid == kbd_owner_pid) {
        kbd_owner_pid = kbd_default_owner_pid;
    }
}

uint64_t keyboard_input_owner(void) {
    return kbd_owner_pid;
}

// Whether the process on this CPU right now is the one allowed to consume a
// keystroke.
static int caller_owns_input(void) {
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
    if (!caller_owns_input()) {
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
    if (!caller_owns_input()) {
        return SYS_WOULD_BLOCK;
    }

    char c;
    if (!keyboard_getchar_nonblock(&c)) {
        return SYS_WOULD_BLOCK;
    }
    return (int64_t)(unsigned char)c;
}

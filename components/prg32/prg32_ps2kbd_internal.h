#ifndef PRG32_PS2KBD_INTERNAL_H
#define PRG32_PS2KBD_INTERNAL_H

// codes returned by prg32_ps2kbd_read_key(), for all the printable characters the ASCII code will be returned
typedef uint8_t ps2kbd_keycode_t;

const ps2kbd_keycode_t PS2KBD_ENTER = 13;
const ps2kbd_keycode_t PS2KBD_ESC = 27;
const ps2kbd_keycode_t PS2KBD_LEFT_ARROW = 37;
const ps2kbd_keycode_t PS2KBD_UP_ARROW = 38;
const ps2kbd_keycode_t PS2KBD_RIGHT_ARROW = 39;
const ps2kbd_keycode_t PS2KBD_DOWN_ARROW = 40;

// initialize PS/2 keyboard driver
void prg32_ps2kbd_init();

// read the current input from the PS/2 keyboard, this is compatible with prg32_input_read(), it returns a bit-mask
// mapping the natural up,down,left,right; 'Z' and 'X' keys are mapped to button 'A' and 'B',
// enter and space are mapped to 'START' and 'ESC' is mapped to 'SELECT'
uint32_t prg32_ps2kbd_controller_read();

#endif

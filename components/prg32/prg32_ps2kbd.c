#include "prg32.h"

#include <esp_log.h>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <driver/gpio.h>
#include <soc/gpio_reg.h>

#include "prg32_ps2kbd_internal.h"

static const int scan_codes_queue_length = 64;
static const int64_t timeout = 250000;
static const int64_t repeated_key_press_delay = 25000;

static const char *TAG = "prg32_ps2kbd";

// make sure to initialize just once
static uint8_t initialized = 0;

// the queue of received scan_codes
static QueueHandle_t scan_codes;

// IRQ count and last received byte just for debug purposes
static volatile uint32_t irq_count = 0;
static volatile uint8_t last_byte_rx;

// keymap from scancode to keycode, just uppercase is fine for now
static const ps2kbd_keycode_t keymap[] = {
    0, 0 /*F9*/, 0, 0 /*F5*/, 0 /*F3*/, 0 /*F1*/, 0 /*F2*/, 0 /*F12*/,
    0, 0 /*F10*/, 0 /*F8*/, 0 /*F6*/, 0 /*F4*/, 0 /*TAB*/, '~', 0,
    0, 0 /*Lalt*/, 0 /*Lshift*/, 0, 0 /*Lctrl*/, 'Q', '!', 0,
    0, 0, 'Z', 'S', 'A', 'W', '@', 0,
    0, 'C', 'X', 'D', 'E', '$', '#', 0,
    0, ' ', 'V', 'F', 'T', 'R', '%', 0,
    0, 'N', 'B', 'H', 'G', 'Y', '^', 0,
    0, 0, 'M', 'J', 'U', '&', '*', 0,
    0, '<', 'K', 'I', 'O', ')', '(', 0,
    0, '>', '?', 'L', ':', 'P', '_', 0,
    0, 0, '"', 0, '{', '+', 0, 0,
    0 /*CapsLock*/, 0 /*Rshift*/, PS2KBD_ENTER, '}', 0, '|', 0, 0,
    0, 0, 0, 0, 0, 0, 0 /*backspace*/, 0,
    0, '1', 0, '4', '7', 0, 0, 0,
    '0', '.', '2', '5', '6', '8', PS2KBD_ESC, 0 /*NumLock*/,
    0 /*F11*/, '+', '3', '-', '*', '9', 0 /*SCROLL*/, 0,
    0, 0, 0, 0 /*F7*/
};

static void IRAM_ATTR ps2kbd_clock_callback(void *arg) {
    // PS/2 keyboard sends a packet of 11 bit, one at each clock we keep the "next" bit index and the packet received
    // so far, we can ignore (for now) bit 0, 9, 10; the useful data is exactly one byte (scan_code)
    static uint8_t bit_index = 0;
    static uint8_t scan_code = 0;
    static int64_t last_irq_us = 0;
    static DRAM_ATTR uint8_t bit = 0;

    (void) arg;

    irq_count++;

    // Extract the bit value for the data pin
    // NOTE: we can't use gpio_get_level() reliably in an IRQ service routine
    const uint32_t gpio_in_reg = REG_READ(GPIO_IN_REG);
    bit = (gpio_in_reg >> CONFIG_PRG32_PS2KBD_PIN_DATA) & 0x01;

    // if the previous interrupt was received more than 250ms we discard the packet so far and start a new one
    // 250ms is more than enough, PS/2 should work between 10 and 16 kHz
    const int64_t now_us = esp_timer_get_time();
    if (now_us - last_irq_us > timeout) {
        // timeout
        bit_index = 0;
        scan_code = 0;
    }
    last_irq_us = now_us;

    // add to current_data bits from 1 to 7
    const uint8_t bit_pos = bit_index - 1;
    if (bit_pos <= 7) {
        scan_code |= (bit << bit_pos);
    }
    bit_index++;

    // last bit of the packet, send it to the queue and wakeup eventual routine waiting on the queue
    if (bit_index == 11) {
        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(scan_codes, &scan_code, &woken);
        if (woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
        last_byte_rx = scan_code;
        bit_index = 0;
        scan_code = 0;
    }
}

static void ps2kbd_debug_task(void *arg) {
    while (true) {
        ESP_LOGI(TAG,
                 "DATA=%d CLOCK=%d IRQ_COUNT=%lu LAST_BYTE_RX=0x%x",
                 gpio_get_level(CONFIG_PRG32_PS2KBD_PIN_DATA),
                 gpio_get_level(CONFIG_PRG32_PS2KBD_PIN_CLOCK),
                 (unsigned long)irq_count,
                 last_byte_rx);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static esp_err_t ps2kbd_init(void) {
    ESP_LOGI(TAG, "init PS/2 keyboard DATA %d CLOCK %d", CONFIG_PRG32_PS2KBD_PIN_DATA, CONFIG_PRG32_PS2KBD_PIN_CLOCK);

    if (initialized) {
        ESP_LOGW(TAG, "PS/2 keyboard already initialized");
        return ESP_OK;
    }

    scan_codes = xQueueCreate(scan_codes_queue_length, sizeof(uint8_t));
    if (scan_codes == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "initialized bytes queue");

    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << CONFIG_PRG32_PS2KBD_PIN_DATA) |
                        (1ULL << CONFIG_PRG32_PS2KBD_PIN_CLOCK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, /* External pull-ups to 3.3 V */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        goto fail;
    }
    ESP_LOGI(TAG, "initialized GPIO");

    // setting interrupt for clock pin
    err = gpio_set_intr_type(CONFIG_PRG32_PS2KBD_PIN_CLOCK, GPIO_INTR_NEGEDGE);
    if (err != ESP_OK) {
        goto fail;
    }

    // initializing per-pin interrupt service
    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        // it might be not OK if already initialized
        goto fail;
    }

    // installing callback for clock interrupt
    err = gpio_isr_handler_add(CONFIG_PRG32_PS2KBD_PIN_CLOCK, ps2kbd_clock_callback, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    ESP_LOGI(TAG, "installed CLOCK callback");

#if defined(PRG32_PS2KBD_DEBUG)
    xTaskCreate(ps2kbd_debug_task, "pskbd2_debug", 2048, NULL, 5, NULL);
#else
    (void) ps2kbd_debug_task;
#endif

    initialized = 1;

    return ESP_OK;

fail:
    vQueueDelete(scan_codes);
    scan_codes = NULL;
    ESP_LOGE(TAG, "PS/2 init failed: %s", esp_err_to_name(err));
    return err;
}

// scan through received scancodes and conver them to keycodes, it will return an ASCII character or a special
// key code for controls
// NOTE: we ignore modifiers such as Shift, CapsLock, Alt

const static uint8_t break_scan_code = 0xF0;
const static uint8_t escape_scan_code = 0xE0; // this is not the ESC key, but it is used for expanded key codes

static ps2kbd_keycode_t get_key_code(void) {
    uint8_t scan_code;
    const ps2kbd_keycode_t no_key_code = 0;
    static bool escape = false;
    static bool key_release = false;
    static uint64_t call_id = 0;
    call_id++;
    while (1) {
        // Retrieve the scan code
        if (xQueueReceive(scan_codes, &scan_code, 0) != pdTRUE) {
            return no_key_code;
        }
        // ESP_LOGI(TAG, "Call %llu -- Received scan code 0x%x", call_id, scan_code);
        if (scan_code == break_scan_code) {
            key_release = true;
        } else if (scan_code == escape_scan_code) {
            escape = true;
        } else if (key_release) {
            // the current key was released, just ignore it for now, later we should handle Ctrl, Shift, Alt ...
            ESP_LOGI(TAG, "Call %llu IRQ count: %lu -- Released '%c'", call_id, irq_count, keymap[scan_code]);
            key_release = false;
        } else if (escape) {
            escape = false;
            switch (scan_code) {
                case 0x75: return PS2KBD_UP_ARROW;
                case 0x6B: return PS2KBD_LEFT_ARROW;
                case 0x72: return PS2KBD_DOWN_ARROW;
                case 0x74: return PS2KBD_RIGHT_ARROW;
                case 0x5A: return PS2KBD_ENTER;
                default:
                    ESP_LOGW(TAG, "Call %llu IRQ count: %lu -- Unhandled escaped scan code 0x%x", call_id, irq_count,
                        scan_code);
            }
        } else {
            // not escaped, just return one from the keymap
            if (scan_code >= sizeof(keymap) / sizeof(keymap[0])) {
                ESP_LOGW(TAG, "Call %llu IRQ count: %lu -- Scan code 0x%x out of boundary of the keymap", call_id,
                    irq_count, scan_code);
            } else {
                ESP_LOGI(TAG, "Call %llu IRQ count: %lu -- Pressed '%c'", call_id, irq_count, keymap[scan_code]);
                return keymap[scan_code];
            }
        }
    }
    return no_key_code;
}

void prg32_ps2kbd_init() {
    ESP_ERROR_CHECK(ps2kbd_init());
}

uint32_t prg32_ps2kbd_controller_read() {
    if (!initialized) {
        return 0;
    }
    uint32_t v = 0;
    for (;;) {
        const uint32_t code = (uint32_t) get_key_code();
        if (code == 0) {
            break;
        }
        if (code == PS2KBD_DOWN_ARROW) {
            v |= PRG32_BTN_DOWN;
        } else if (code == PS2KBD_UP_ARROW) {
            v |= PRG32_BTN_UP;
        } else if (code == PS2KBD_LEFT_ARROW) {
            v |= PRG32_BTN_LEFT;
        } else if (code == PS2KBD_RIGHT_ARROW) {
            v |= PRG32_BTN_RIGHT;
        } else if (code == PS2KBD_ENTER || code == ' ') {
            v |= PRG32_BTN_START;
        } else if (code == PS2KBD_ESC) {
            v |= PRG32_BTN_SELECT;
        } else if (code == 'Z' || code =='z') {
            v |= PRG32_BTN_A;
        } else if (code == 'X' || code == 'x') {
            v |= PRG32_BTN_B;
        }
    }
    if (v != 0) {
        ESP_LOGI(TAG, "read 0x%lx", v);
    }
    return v;
}

uint16_t prg32_ps2kbd_read_key(void) {
    if (!initialized) {
        return 0;
    }
    static ps2kbd_keycode_t previous_key = 0;
    static int64_t previous_key_ts = 0;
    const ps2kbd_keycode_t key_code = get_key_code();
    if (key_code == 0) {
        // no key code in buffer
        return 0;
    }
    const int64_t now = esp_timer_get_time();
    if (key_code == previous_key) {
        // let's debounce a little forcing a 250ms
        if (now - previous_key_ts < repeated_key_press_delay) {
            return 0;
        }
    }
    // ESP_LOGI(TAG, "read 0x%x (%c) after %lli us", key_code, (char) key_code, now - previous_key_ts);
    previous_key = key_code;
    previous_key_ts = now;
    return key_code;
}


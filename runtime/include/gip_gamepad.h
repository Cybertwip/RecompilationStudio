#ifndef PSXRECOMP_GIP_GAMEPAD_H
#define PSXRECOMP_GIP_GAMEPAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_GIP_SELECTOR_CAPACITY 128
#define PSX_GIP_NAME_CAPACITY     128

/* Normalized button word carried by Xbox GIP command 0x20. */
enum {
    PSX_GIP_BUTTON_MENU          = 0x0004,
    PSX_GIP_BUTTON_VIEW          = 0x0008,
    PSX_GIP_BUTTON_A             = 0x0010,
    PSX_GIP_BUTTON_B             = 0x0020,
    PSX_GIP_BUTTON_X             = 0x0040,
    PSX_GIP_BUTTON_Y             = 0x0080,
    PSX_GIP_BUTTON_DPAD_UP       = 0x0100,
    PSX_GIP_BUTTON_DPAD_DOWN     = 0x0200,
    PSX_GIP_BUTTON_DPAD_LEFT     = 0x0400,
    PSX_GIP_BUTTON_DPAD_RIGHT    = 0x0800,
    PSX_GIP_BUTTON_LEFT_BUMPER   = 0x1000,
    PSX_GIP_BUTTON_RIGHT_BUMPER  = 0x2000,
    PSX_GIP_BUTTON_LEFT_STICK    = 0x4000,
    PSX_GIP_BUTTON_RIGHT_STICK   = 0x8000
};

typedef struct PsxGipGamepadState {
    uint16_t buttons;
    uint8_t guide;
    uint16_t left_trigger;   /* 0..1023 */
    uint16_t right_trigger;  /* 0..1023 */
    int16_t left_x;
    int16_t left_y;          /* SDL convention: negative up, positive down */
    int16_t right_x;
    int16_t right_y;         /* SDL convention: negative up, positive down */
} PsxGipGamepadState;

typedef struct PsxGipGamepadInfo {
    char selector[PSX_GIP_SELECTOR_CAPACITY];
    char name[PSX_GIP_NAME_CAPACITY];
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t bus;
    uint8_t address;
} PsxGipGamepadInfo;

typedef enum PsxGipGamepadConnection {
    PSX_GIP_CONNECTION_SEARCHING = 0,
    PSX_GIP_CONNECTION_INITIALIZING = 1,
    PSX_GIP_CONNECTION_CONNECTED = 2,
    PSX_GIP_CONNECTION_UNAVAILABLE = 3
} PsxGipGamepadConnection;

typedef struct PsxGipGamepad PsxGipGamepad;

/* Returns the total number of supported devices. At most `capacity` entries are
 * copied into `out`; pass NULL/0 to count only. */
size_t psx_gip_gamepad_enumerate(PsxGipGamepadInfo* out, size_t capacity);

/* True for selectors emitted by psx_gip_gamepad_enumerate and for "gip:auto". */
int psx_gip_gamepad_selector_supported(const char* selector);

/* Starts a reconnecting background reader. The object remains valid while a
 * selected controller is unplugged and resumes when a matching device returns. */
PsxGipGamepad* psx_gip_gamepad_open(const char* selector);
void psx_gip_gamepad_close(PsxGipGamepad* gamepad);
PsxGipGamepadConnection psx_gip_gamepad_connection(const PsxGipGamepad* gamepad);

/* Copies the latest coherent state. Returns 1 while the USB link is connected,
 * 0 while searching/initializing; disconnected state is always neutral. */
int psx_gip_gamepad_get_state(const PsxGipGamepad* gamepad,
                              PsxGipGamepadState* out);

/* Pure protocol helpers used by the USB backend and conformance test. */
size_t psx_gip_gamepad_build_ack(uint8_t host_sequence,
                                 const uint8_t* packet,
                                 size_t packet_size,
                                 uint8_t out_ack[13]);

/* Updates `state` for input (0x20) or guide (0x07) packets.
 * Returns 1 when state changed, 0 for an unrelated command, -1 for malformed
 * input/guide packets. */
int psx_gip_gamepad_parse_packet(const uint8_t* packet,
                                 size_t packet_size,
                                 PsxGipGamepadState* state);

#ifdef __cplusplus
}
#endif

#endif

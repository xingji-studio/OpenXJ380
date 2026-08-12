#pragma once

#include <stdint.h>

enum OpenXJ380InputSource
{
    OPENXJ380_INPUT_SOURCE_PS2 = 0,
    OPENXJ380_INPUT_SOURCE_USB = 1,
};

enum OpenXJ380InputRoute
{
    OPENXJ380_INPUT_ROUTE_DECODED = 0,
    OPENXJ380_INPUT_ROUTE_XJ380_PS2_IRQ = 1,
    OPENXJ380_INPUT_ROUTE_XJ380_USB = 2,
};

/*
 * The provider kernel owns PS/2 decoding and interrupt acknowledgement.  The
 * product layer receives a value-only event so it cannot touch controller I/O
 * from a loadable module.
 */
typedef struct OpenXJ380MouseInterruptInfo
{
    void *regs;
    uint64_t error_code;
    uint8_t source;
    uint8_t route;
    uint8_t packet_byte;
    uint8_t packet_complete;
    uint8_t buttons;
    int16_t delta_x;
    int16_t delta_y;
    int16_t wheel;
    int32_t x;
    int32_t y;
} OpenXJ380MouseInterruptInfo;

typedef struct OpenXJ380KeyboardInterruptInfo
{
    void *regs;
    uint64_t error_code;
    uint8_t source;
    uint8_t route;
    uint8_t usb_usage;
    uint8_t raw_scancode;
    uint8_t make_code;
    uint8_t value;
    uint8_t message_type;
    uint8_t extended;
    uint8_t pressed;
    uint8_t shift;
    uint8_t ctrl;
    uint8_t alt;
    uint8_t win;
    uint8_t caps;
} OpenXJ380KeyboardInterruptInfo;

typedef bool (*OpenXJ380MouseInterruptHook)(const OpenXJ380MouseInterruptInfo *event);
typedef bool (*OpenXJ380KeyboardInterruptHook)(const OpenXJ380KeyboardInterruptInfo *event);

#ifdef __cplusplus
extern "C" {
#endif

bool OpenXJ380Socket_MouseInterrupte(const OpenXJ380MouseInterruptInfo *event);
bool OpenXJ380Socket_KeyboardInterrupt(const OpenXJ380KeyboardInterruptInfo *event);

int OpenXJ380Socket_RegisterMouseHook(OpenXJ380MouseInterruptHook hook);
int OpenXJ380Socket_RegisterKeyboardHook(OpenXJ380KeyboardInterruptHook hook);
void OpenXJ380Socket_UnregisterMouseHook(OpenXJ380MouseInterruptHook hook);
void OpenXJ380Socket_UnregisterKeyboardHook(OpenXJ380KeyboardInterruptHook hook);
const void *OpenXJ380Socket_FramebufferConfig();
uint64_t OpenXJ380Socket_PowerAction(uint64_t action);

#ifdef __cplusplus
}
#endif

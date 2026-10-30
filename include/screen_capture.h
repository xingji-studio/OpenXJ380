#pragma once

#include <task/pcb.h>

// A successful SetSecret call lasts until process teardown, including across exec.
int screen_capture_set_secret();
int screen_capture_begin();
void screen_capture_end();
void screen_capture_register_mapping();
void screen_capture_process_inherit(pcb_t parent, pcb_t child);
void screen_capture_process_exit(pcb_t process);

// Product kernel implementation: submits a trusted ALERT, never a user notice.
int screen_capture_post_notice();

class ScreenCaptureAccess
{
public:
    const int result;
    ScreenCaptureAccess() : result(screen_capture_begin()) {}
    ~ScreenCaptureAccess() { if (result == 0) screen_capture_end(); }
    ScreenCaptureAccess(const ScreenCaptureAccess &) = delete;
    ScreenCaptureAccess &operator=(const ScreenCaptureAccess &) = delete;
};

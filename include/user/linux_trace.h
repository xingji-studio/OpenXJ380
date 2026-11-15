#pragma once

// Verbose argv/path traces can throttle exec and completion on a slow UART.
// Keep normal boot/error logging; enable these ABI diagnostics explicitly in
// compiler flags when debugging a Linux userspace compatibility issue.
#ifndef CONFIG_TRACE_LINUX_ABI
#define CONFIG_TRACE_LINUX_ABI 0
#endif

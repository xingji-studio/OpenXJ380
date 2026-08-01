#pragma once

// Target platform.
// Define XJ380, STARDUSTUI_WINDOWS, or STARDUSTUI_LINUX before including this
// file to override the automatic default.
#if !defined(XJ380) && !defined(STARDUSTUI_WINDOWS) && !defined(STARDUSTUI_LINUX)
#if defined(_WIN32)
#define STARDUSTUI_WINDOWS
#elif defined(__linux__)
#define STARDUSTUI_LINUX
#else
#define XJ380
#endif
#endif

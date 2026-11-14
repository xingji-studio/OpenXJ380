#pragma once

#include "build_settings.h"
#include <efi/fbc.h>

namespace boot_debug {

// Read left to right, then top to bottom, with 16 fixed slots per row.
// A colored left half means entered; a full colored cell means returned (not
// necessarily successful). Gray means skipped; dark means not reached.
// Keep these slots stable across boot options and conditional compilation.
enum class Step : uint32_t
{
    BootConfig = 0,          // Row 0: slots 0-15.
    Console,
    Cpu,
    Idt,
    Gdt,
    Hpet,
    Apic,
    Hhdm,
    InstallerBoot,
    Frames,
    Heap,
    DeviceManager,
    Vfs,
    FatFs,
    Ext4Fs,
    DevFs,
    PciSetup = 16,           // Row 1: slots 16-31.
    PciInit,
    Rng,
    Ahci,
    Nvme,
    Ide,
    Xhci,
    Partitions,
    Audio,
    Keyboard,
    Mouse,
    Processes,
    SmpParameters,
    Smp,
    IdleThread,
    LowMappings,
    XhciWorkers = 32,        // Row 2: slots 32-47.
    InstallerPackages,
    InstallerSource,
    InstallerRootPrepare,
    InstallerRootPublish,
    InstallerRootCheck,
    MountRoot,
    BusyboxAliases,
    XbpsAliases,
    TmpFs,
    PipeMountpoint,
    PipeFs,
    PipeCheck,
    Pty,
    Fsgsbase,
    SocketMountpoint,
    SocketFs = 48,           // Row 3: slots 48-63.
    SocketCheck,
    ProcMountpoint,
    ProcFs,
    ProcCheck,
    DnsFs,
    NmFs,
    Stdio,
    Signals,
    Syscalls,
    Xnet,
    Messages,
    Modules,
    Dlinker,
    LoadModules,
    SmpReady,
    Reaper = 64,             // Row 4: slots 64-73.
    SerialRuntime,
    StoragePolicy,
    InstallerEntry,
    UserSession,
    UserProcess,
    Scheduler,
    BootLogo,               // Product-only steps; preserve the provider slot numbers.
    InputMethod,
    AudioRuntime,
    Count
};

#if DEVELOP_MODE_BOOT_DEBUG_COLOR == 1
uint32_t reserved_height(const FrameBufferConfig &fbc);
void initialize(const FrameBufferConfig &fbc);
void begin(const FrameBufferConfig &fbc, Step step);
void returned(const FrameBufferConfig &fbc, Step step);
void skip(const FrameBufferConfig &fbc, Step first, Step last);
#else
inline uint32_t reserved_height(const FrameBufferConfig &)
{
    return 0;
}

inline void initialize(const FrameBufferConfig &) {}

inline void begin(const FrameBufferConfig &, Step) {}

inline void returned(const FrameBufferConfig &, Step) {}

inline void skip(const FrameBufferConfig &, Step, Step) {}
#endif

inline void skip(const FrameBufferConfig &fbc, Step step)
{
    skip(fbc, step, step);
}

} // namespace boot_debug

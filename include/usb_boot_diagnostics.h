#pragma once
// SPDX-License-Identifier: Apache-2.0
#include <stdint.h>

enum class UsbBootStage : uint32_t
{
    Controller,
    PortConnect,
    PortReset,
    EnableSlot,
    Address,
    Descriptor8,
    Descriptor,
    Configuration,
    SetConfiguration,
    DriverProbe,
    MscEndpoints,
    MscReady,
    MscCapacity,
    MscRegister
};

// Shared by the driver and startup UI, independent of surrounding headers.
#pragma pack(push, 8)
struct UsbBootProbe
{
    uint32_t port;
    UsbBootStage stage;
    int32_t result;
    UsbBootStage failed_stage;
    int32_t first_error;
    uint32_t completion_code, timed_out, request, request_type, transfer_bytes, opcode, scsi_status;
    uint16_t vendor, product;
    uint8_t interface_class, interface_subclass, interface_protocol;
};

struct UsbBootDiagnostics
{
    uint32_t ports, connected, expected_port, expected_portsc, command, status;
    uint64_t connected_mask;
    UsbBootProbe expected, last;
};
#pragma pack(pop)

// Boot-only observations, never authority for matching or opening a source.
// Probe ports are one-based native xHCI root-port numbers.
bool xhci_boot_diagnostics(UsbBootDiagnostics *output);

static inline const char *usb_boot_stage_name(UsbBootStage stage)
{
    switch (stage)
    {
    case UsbBootStage::Controller: return "controller";
    case UsbBootStage::PortConnect: return "port-connect";
    case UsbBootStage::PortReset: return "port-reset";
    case UsbBootStage::EnableSlot: return "enable-slot";
    case UsbBootStage::Address: return "address";
    case UsbBootStage::Descriptor8: return "descriptor-8";
    case UsbBootStage::Descriptor: return "descriptor";
    case UsbBootStage::Configuration: return "config-read";
    case UsbBootStage::SetConfiguration: return "set-config";
    case UsbBootStage::DriverProbe: return "driver-probe";
    case UsbBootStage::MscEndpoints: return "msc-endpoints";
    case UsbBootStage::MscReady: return "msc-ready";
    case UsbBootStage::MscCapacity: return "msc-capacity";
    case UsbBootStage::MscRegister: return "msc-register";
    }
    return "unknown";
}

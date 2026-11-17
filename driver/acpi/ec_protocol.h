#pragma once

#include <stdint.h>

// The transport is independent of AML and port addresses so timeout/order
// behavior can be exercised against a controller model on the host.
struct AcpiEcTransport
{
    void *context;
    uint8_t (*status)(void *);
    uint8_t (*read_data)(void *);
    void (*write_command)(void *, uint8_t);
    void (*write_data)(void *, uint8_t);
    uint64_t (*now_ns)(void *);
    void (*stall)(void *);
};

static bool acpi_ec_wait(const AcpiEcTransport &io, uint8_t mask, uint8_t value)
{
    uint64_t start = io.now_ns(io.context);
    // One controller handshake is bounded to 500 ms. A second finite bound
    // also rejects a stopped clock instead of hanging the power action.
    for (unsigned polls = 0; polls != 500000; ++polls)
    {
        if ((io.status(io.context) & mask) == value) return true;
        uint64_t now = io.now_ns(io.context);
        if (now < start || now - start >= 500000000ULL) return false;
        io.stall(io.context);
    }
    return false;
}

static bool acpi_ec_read_byte(const AcpiEcTransport &io, uint8_t address, uint8_t *out)
{
    if (!acpi_ec_wait(io, 2, 0)) return false; // IBF clear before every host write.
    io.write_command(io.context, 0x80);
    if (!acpi_ec_wait(io, 2, 0)) return false;
    io.write_data(io.context, address);
    if (!acpi_ec_wait(io, 1, 1)) return false; // OBF set before consuming a byte.
    *out = io.read_data(io.context);
    return true;
}

static bool acpi_ec_write_byte(const AcpiEcTransport &io, uint8_t address, uint8_t value)
{
    if (!acpi_ec_wait(io, 2, 0)) return false;
    io.write_command(io.context, 0x81);
    if (!acpi_ec_wait(io, 2, 0)) return false;
    io.write_data(io.context, address);
    if (!acpi_ec_wait(io, 2, 0)) return false;
    io.write_data(io.context, value);
    return acpi_ec_wait(io, 2, 0);
}

static bool acpi_ec_query(const AcpiEcTransport &io, uint8_t *query)
{
    if (!acpi_ec_wait(io, 2, 0)) return false;
    io.write_command(io.context, 0x84);
    if (!acpi_ec_wait(io, 1, 1)) return false;
    *query = io.read_data(io.context);
    return true;
}

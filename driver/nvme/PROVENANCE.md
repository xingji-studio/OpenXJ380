# NVMe flush and completion changes

The flush, completion validation, timeout, and DMA-lifetime changes are
first-party implementation code under the repository's Apache-2.0 license.
No external NVMe driver implementation was imported or translated. Tests compile
the production driver against an independently written emulated controller.

Protocol references are the public NVM Express specifications, owned by
NVM Express, Inc.: © 2008 to 2024 NVM Express, Inc. ALL RIGHTS RESERVED.
These references describe wire/register facts; neither specification text nor
external driver source is embedded in the kernel implementation.

- [NVM Express Base Specification 2.0e](https://nvmexpress.org/wp-content/uploads/NVM-Express-Base-Specification-2.0e-2024.07.29-Ratified.pdf): submission/completion queues, controller disable and ready transitions, command and completion identifiers, and Flush command ordering.
- [NVM Command Set Specification 1.0e](https://nvmexpress.org/wp-content/uploads/NVM-Express-NVM-Command-Set-Specification-1.0e-2024.07.29-Ratified.pdf): NVM Flush and read/write command fields, including NLB, Limited Retry, and FUA.

`IOBLKSYNC` now submits an actual namespace-specific NVM Flush command through
the same serialized I/O submission path used for reads and writes. It has no
PRPs, metadata buffer, or command-specific data words. A successful completion
requires both SC and SCT to be zero. A prior failed write or flush remains an
error until controller reinitialization; the driver does not hide it with a
later successful no-op.

Timeout and fatal/protocol failures take the controller offline and attempt
CC.EN=0, then wait for CSTS.RDY=0 using CAP.TO. I/O uses driver-owned bounded
bounce buffers. If the controller does not acknowledge disable, outstanding
DMA buffers/PRP lists and queue allocations remain pinned; no later command is
submitted. This intentionally trades bounded retained memory for avoiding DMA
into freed or returned caller memory. Automatic recovery, PCI bus-master
isolation, hot unplug, and namespace re-enumeration are not implemented here.

Host ASan/UBSan tests cover real production command construction, status types,
queue/phase wrap, CID/QID validation, deadlines, sticky errors, successful and
failed controller quiescence, and late DMA into retained owned buffers. Physical
power-loss behavior remains subject to the device's implementation of Flush.

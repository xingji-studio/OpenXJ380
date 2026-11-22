// SPDX-License-Identifier: Apache-2.0
#include "source_media.h"
#include <installer/source_reader.h>
#include <krlibc.h>
#include <proto.hpp>

namespace installer {
namespace {
// One boot-owned installer session. Storage is bounded independently of file
// data size, becomes private immutable metadata at publication, and survives
// a worker death or source quarantine until reboot.
SourceMedia        media;
SourceReadContext *owner;

bool device_read(void *opaque, uint64_t offset, void *output, size_t bytes)
{
    int error = read_source_device(opaque, offset, output, bytes);
    if (error)
        write_serial_fmt_heapless("installer: file device read offset=%llu bytes=%llu error=%d\n",
                                  (unsigned long long)offset, (unsigned long long)bytes, error);
    return error == 0;
}

int files_read(void *opaque, uint64_t offset, void *output, size_t bytes)
{
    auto  &reader = *static_cast<SourceReadContext *>(opaque);
    size_t done   = 0;
    while (done < bytes)
    {
        unsigned file   = 0;
        uint64_t within = 0;
        size_t   amount = 0;
        int      error =
            xjinstaller::stream_index_file_location(*reader.index, offset + done, bytes - done, file, within, amount);
        if (!error) error = media.read(file, within, static_cast<uint8_t *>(output) + done, amount);
        if (error) return error;
        done += amount;
    }
    return 0;
}
} // namespace

int prepare_stream_source(SourceReadContext &reader)
{
    if (!reader.index || reader.index->version != 3 || !reader.source || owner || reader.files.read) return -22;
    owner                = &reader;
    const auto &identity = reader.source->identity;
    reader.preparing     = true;
    int result           = media.open(device_read, &reader, identity.physical_bytes, identity.volume_offset,
                                      identity.volume_bytes, *reader.index);
    reader.preparing     = false;
    if (result)
    {
        write_serial_fmt_heapless(
            "installer: file source failed stage=%s error=%d volume_offset=%llu volume_bytes=%llu\n",
            media.diagnostic(), result, (unsigned long long)identity.volume_offset,
            (unsigned long long)identity.volume_bytes);
        return result;
    }
    reader.files = {files_read, &reader, reader.index->data_bytes};
    write_serial_fmt_heapless("installer: file source=%s files=%llu bytes=%llu\n", media.is_iso() ? "iso9660" : "fat32",
                              (unsigned long long)reader.index->extent_count,
                              (unsigned long long)reader.index->data_bytes);
    return 0;
}
} // namespace installer

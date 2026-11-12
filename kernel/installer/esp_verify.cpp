// SPDX-License-Identifier: Apache-2.0
// Original bounded EFI FAT verification from public on-disk format facts.
// No FatFs/Linux/libfat implementation is copied into this independent reader.
#include <installer/esp_verify.h>
#include <errno.h>

namespace xjinstaller {
namespace {

static uint16_t le16(const uint8_t *p)
{
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return uint32_t(le16(p)) | (uint32_t(le16(p + 2)) << 16);
}

static bool same(const void *a, const void *b, EspVerifySize bytes)
{
    const auto *x = static_cast<const uint8_t *>(a), *y = static_cast<const uint8_t *>(b);
    for (EspVerifySize i = 0; i < bytes; ++i) if (x[i] != y[i]) return false;
    return true;
}

static bool zero(const uint8_t *p, EspVerifySize bytes)
{
    for (EspVerifySize i = 0; i < bytes; ++i) if (p[i]) return false;
    return true;
}

static void clear(void *p, EspVerifySize bytes)
{
    auto *data = static_cast<uint8_t *>(p);
    for (EspVerifySize i = 0; i < bytes; ++i) data[i] = 0;
}

struct Span { const void *data; EspVerifySize bytes; };

static bool valid_span(Span span)
{
    uintptr_t base = reinterpret_cast<uintptr_t>(span.data);
    return base && span.bytes && span.bytes <= ~uintptr_t(0) - base;
}

static bool disjoint(Span a, Span b)
{
    uintptr_t x = reinterpret_cast<uintptr_t>(a.data), y = reinterpret_cast<uintptr_t>(b.data);
    return x >= y ? x - y >= b.bytes : y - x >= a.bytes;
}

static const char names[8][12] = {
    "", "EFI        ", "SYSTEM     ", "BOOT       ", "XJ380      ",
    "BOOTX64 EFI", "KERNEL  KRL", "ROOT    BIN"
};
static const unsigned parents[8] = {0, 0, 0, 1, 1, 3, 2, 4};

class Verifier
{
    const EspReadDevice &device;
    const EspExpectedFiles &files;
    EspVerifyWorkspace &work;
    uint32_t total = 0, reserved = 0, fat_sectors = 0, cluster_sectors = 0, data_start = 0, clusters = 0;
    uint32_t start[8] = {}, cached_fat = uint32_t(-1), fsinfo_free = 0;

    int sector(uint64_t number, uint8_t *output)
    {
        if (work.io_failed) return -EIO;
        if (number >= device.bytes / 512) return -EINVAL;
        work.started = true;
        ++work.read_requests;
        if (!device.read(device.context, number * 512, output, 512))
        {
            work.io_failed = true;
            return -EIO;
        }
        return 0;
    }

    bool cluster_valid(uint32_t cluster) const
    {
        return cluster >= 2 && cluster <= clusters + 1;
    }

    int take(uint32_t cluster)
    {
        if (!cluster_valid(cluster)) return -EINVAL;
        uint8_t &bits = work.cluster_bits[cluster / 8];
        uint8_t mask = uint8_t(1U << (cluster % 8));
        if (bits & mask) return -EINVAL; // Cycle or cross-linked object.
        bits |= mask;
        return 0;
    }

    uint64_t first_sector(uint32_t cluster) const
    {
        return uint64_t(data_start) + uint64_t(cluster - 2) * cluster_sectors;
    }

    int load_fat(uint32_t number)
    {
        if (number >= fat_sectors) return -EINVAL;
        if (number == cached_fat) return 0;
        int error = sector(uint64_t(reserved) + number, work.fat);
        if (!error) error = sector(uint64_t(reserved) + fat_sectors + number, work.mirror);
        if (error) return error;
        if (!same(work.fat, work.mirror, 512)) return -EINVAL;
        cached_fat = number;
        return 0;
    }

    int fat_next(uint32_t cluster, uint32_t &next)
    {
        if (!cluster_valid(cluster)) return -EINVAL;
        int error = load_fat(cluster / 128);
        if (!error) next = le32(work.fat + (cluster % 128) * 4) & 0x0fffffffU;
        return error;
    }

    int fsinfo(uint32_t number, bool primary)
    {
        int error = sector(number, work.data);
        if (error) return error;
        if (le32(work.data) != 0x41615252 || le32(work.data + 484) != 0x61417272 ||
            le32(work.data + 508) != 0xaa550000 || !zero(work.data + 4, 480) ||
            !zero(work.data + 496, 12)) return -EINVAL;
        uint32_t free = le32(work.data + 488), hint = le32(work.data + 492);
        if ((free != uint32_t(-1) && free > clusters) ||
            (hint != uint32_t(-1) && !cluster_valid(hint))) return -EINVAL;
        if (primary) fsinfo_free = free;
        return 0;
    }

    int geometry()
    {
        int error = sector(0, work.boot);
        if (error) return error;
        const uint8_t *b = work.boot;
        cluster_sectors = b[13]; reserved = le16(b + 14); total = le32(b + 32); fat_sectors = le32(b + 36);
        uint64_t data = uint64_t(reserved) + uint64_t(2) * fat_sectors;
        // This verifier accepts only the mirrored fresh FAT32 factory profile.
        if (le16(b + 11) != 512 || !cluster_sectors || cluster_sectors > 128 ||
            (cluster_sectors & (cluster_sectors - 1)) || reserved < 9 || b[16] != 2 ||
            le16(b + 17) || le16(b + 19) || le16(b + 22) || le16(b + 40) || le16(b + 42) ||
            !total || total > device.bytes / 512 || !fat_sectors || data >= total ||
            le16(b + 48) != 1 || le16(b + 50) != 6 || !zero(b + 52, 12) ||
            b[21] != 0xf8 || b[510] != 0x55 || b[511] != 0xaa) return -EINVAL;
        data_start = uint32_t(data);
        clusters = (total - data_start) / cluster_sectors;
        if (clusters < 65525 || clusters + 1 >= 0x0ffffff0U ||
            uint64_t(fat_sectors) * 128 < uint64_t(clusters) + 2) return -EINVAL;
        start[0] = le32(b + 44);
        if (!cluster_valid(start[0])) return -EINVAL;
        error = sector(6, work.data);
        if (error) return error;
        if (!same(work.boot, work.data, 512)) return -EINVAL;
        error = fsinfo(1, true);
        if (!error) error = fsinfo(7, false); // Backup counters may be older.
        return error;
    }

    int directory(unsigned index)
    {
        int error = take(start[index]);
        if (error) return error;
        uint32_t next = 0;
        error = fat_next(start[index], next);
        // All five fixed directories fit one fresh cluster. Refuse unexpected
        // growth rather than walk an unbounded hidden namespace before boot.
        if (error || next < 0x0ffffff8U) return error ? error : -EINVAL;
        bool ended = false, label = false;
        unsigned dots = 0;
        for (uint32_t s = 0; s < cluster_sectors; ++s)
        {
            error = sector(first_sector(start[index]) + s, work.data);
            if (error) return error;
            for (unsigned slot = 0; slot < 16; ++slot)
            {
                const uint8_t *e = work.data + slot * 32;
                if (!e[0] || ended)
                {
                    ended = true;
                    if (!zero(e, 32)) return -EINVAL;
                    continue;
                }
                uint32_t cluster = (uint32_t(le16(e + 20)) << 16) | le16(e + 26);
                uint32_t bytes = le32(e + 28);
                if (e[12] & ~0x18U || e[0] == 0xe5 || e[11] == 0x0f) return -EINVAL;
                if (index && s == 0 && slot < 2)
                {
                    static const char dot[2][12] = {".          ", "..         "};
                    uint32_t expected = slot == 0 ? start[index] : (parents[index] ? start[parents[index]] : 0);
                    if (!same(e, dot[slot], 11) || e[11] != 0x10 || bytes || cluster != expected) return -EINVAL;
                    ++dots;
                    continue;
                }
                if (!index && e[11] == 8 && !cluster && !bytes && !label)
                {
                    label = true;
                    continue;
                }
                unsigned child = 1;
                while (child < 8 && (parents[child] != index || !same(e, names[child], 11))) ++child;
                if (child == 8 || start[child] || !cluster_valid(cluster)) return -EINVAL;
                uint64_t expected = child < 5 ? 0 : child == 5 ? files.loader_bytes :
                                      child == 6 ? files.kernel_bytes : XJ_ROOT_BINDING_SIZE;
                if (e[11] != (child < 5 ? 0x10 : 0x20) || bytes != expected) return -EINVAL;
                start[child] = cluster;
            }
        }
        if (index && dots != 2) return -EINVAL;
        for (unsigned child = 1; child < 8; ++child)
            if (parents[child] == index && !start[child]) return -ENOENT;
        return 0;
    }

    int file(unsigned index, const uint8_t *expected, uint64_t bytes)
    {
        uint32_t cluster = start[index];
        uint64_t left = bytes, position = 0;
        while (left)
        {
            int error = take(cluster);
            if (error) return error;
            for (uint32_t s = 0; s < cluster_sectors && left; ++s)
            {
                error = sector(first_sector(cluster) + s, work.data);
                if (error) return error;
                EspVerifySize part = left < 512 ? EspVerifySize(left) : 512;
                if (expected ? !same(work.data, expected + position, part) : !zero(work.data, part)) return -EINVAL;
                if (index == 7 && position == 0)
                    for (unsigned i = 0; i < 512; ++i) work.binding_sector[i] = work.data[i];
                left -= part; position += part;
            }
            uint32_t next = 0;
            error = fat_next(cluster, next);
            if (error) return error;
            if (left ? !cluster_valid(next) : next < 0x0ffffff8U) return -EINVAL;
            cluster = next;
        }
        return 0;
    }

    int allocation()
    {
        uint32_t free = 0;
        for (uint32_t cluster = 0; cluster < clusters + 2; ++cluster)
        {
            int error = load_fat(cluster / 128);
            if (error) return error;
            uint32_t raw = le32(work.fat + (cluster % 128) * 4), value = raw & 0x0fffffffU;
            if (cluster == 0) { if (value != 0x0ffffff8U) return -EINVAL; continue; }
            if (cluster == 1) { if (value != 0x0fffffffU) return -EINVAL; continue; }
            bool used = (work.cluster_bits[cluster / 8] & (1U << (cluster % 8))) != 0;
            if (!used && raw) return -EINVAL; // Lost or unlisted object.
            if (used && !(cluster_valid(value) || value >= 0x0ffffff8U)) return -EINVAL;
            if (!used) ++free;
        }
        // A FAT may have whole padding sectors beyond its final data-cluster
        // entry. Check both complete copies, including that padding, instead
        // of silently accepting an unchecked trailing mirror difference.
        uint64_t used_bytes = uint64_t(clusters + 2) * 4;
        for (uint64_t at = used_bytes; at < uint64_t(fat_sectors) * 512;)
        {
            int error = load_fat(uint32_t(at / 512));
            if (error) return error;
            unsigned offset = unsigned(at % 512);
            if (!zero(work.fat + offset, 512 - offset)) return -EINVAL;
            at += 512 - offset;
        }
        return fsinfo_free == uint32_t(-1) || fsinfo_free == free ? 0 : -EINVAL;
    }

public:
    Verifier(const EspReadDevice &device, const EspExpectedFiles &files, EspVerifyWorkspace &work)
        : device(device), files(files), work(work) {}

    int run(EspPublication &publication, const XJ_ROOT_BINDING &binding)
    {
        int error = geometry();
        for (unsigned i = 0; !error && i < 5; ++i) error = directory(i);
        if (!error) error = file(5, files.loader, files.loader_bytes);
        if (!error) error = file(6, files.kernel, files.kernel_bytes);
        if (!error) error = file(7, nullptr, XJ_ROOT_BINDING_SIZE);
        if (!error) error = allocation();
        if (error) return error;
        publication.sector_offset = first_sector(start[7]) * 512;
        for (unsigned i = 0; i < 512; ++i)
            publication.sector[i] = i < XJ_ROOT_BINDING_SIZE ? binding.bytes[i] : work.binding_sector[i];
        work.complete = publication.ready = true;
        return 0;
    }
};

} // namespace

EspVerifySize esp_verify_bitmap_bytes(uint64_t bytes)
{
    if (bytes < 64ULL * 1048576 || bytes % 512 || bytes / 512 > uint32_t(-1)) return 0;
    return EspVerifySize((bytes / 512 + 2 + 7) / 8);
}

int esp_verify_prepare_publication(const EspReadDevice &device, const EspExpectedFiles &files,
                                   const XJ_ROOT_BINDING &binding, EspVerifyWorkspace &work,
                                   EspPublication &publication)
{
    EspVerifySize required = esp_verify_bitmap_bytes(device.bytes);
    Span spans[] = {{&device, sizeof(device)}, {&files, sizeof(files)}, {&binding, sizeof(binding)},
                    {files.loader, files.loader_bytes}, {files.kernel, files.kernel_bytes},
                    {&work, sizeof(work)}, {work.cluster_bits, work.cluster_bits_bytes},
                    {&publication, sizeof(publication)}};
    for (unsigned i = 0; i < sizeof(spans) / sizeof(spans[0]); ++i)
    {
        if (!valid_span(spans[i])) return -EINVAL;
        for (unsigned j = 0; j < i; ++j)
            // Immutable inputs may share storage, but never writable buffers.
            if ((i >= 5 || j >= 5) && !disjoint(spans[i], spans[j])) return -EINVAL;
    }
    if (!required || !device.read || files.loader_bytes > uint32_t(-1) || files.kernel_bytes > uint32_t(-1) ||
        files.loader_bytes > device.bytes || files.kernel_bytes > device.bytes ||
        work.cluster_bits_bytes < required) return -EINVAL;
    if (work.started) return work.io_failed ? -EIO : -EBUSY;
    clear(&publication, sizeof(publication));
    if (!xj_root_binding_kernel_matches(&binding, files.kernel, files.kernel_bytes)) return -EINVAL;
    for (unsigned i = 0; i < 4; ++i)
    {
        const uint8_t *identity = binding.bytes + 16 + i * 16;
        if (zero(identity, 16)) return -EINVAL;
        if (i < 3)
            for (unsigned j = 0; j < i; ++j)
                if (same(identity, binding.bytes + 16 + j * 16, 16)) return -EINVAL;
    }
    clear(work.cluster_bits, required);
    work.read_requests = 0; work.io_failed = false; work.complete = false;
    Verifier verifier(device, files, work);
    return verifier.run(publication, binding);
}

} // namespace xjinstaller

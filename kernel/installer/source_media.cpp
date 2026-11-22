// SPDX-License-Identifier: Apache-2.0
// Original FAT32 carrier reader using public on-disk format facts. No mounts,
// allocation, firmware callbacks, writable filesystem or external parser.
#include "source_media.h"
#include <errno.h>

namespace installer {
namespace {
uint16_t le16(const uint8_t *p)
{
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}

uint32_t le32(const uint8_t *p)
{
    return uint32_t(le16(p)) | uint32_t(le16(p + 2)) << 16;
}

uint64_t le64(const uint8_t *p)
{
    return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32;
}

unsigned lower(unsigned c)
{
    return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

bool same_name(const char *a, const char *b)
{
    while (*a && lower(unsigned(*a)) == lower(unsigned(*b)))
    {
        ++a;
        ++b;
    }
    return !*a && !*b;
}

const char *paths[] = {
    "installer/system-payload.pak",
    "installer/boot-files/EFI/XJ380/BOOTX64.EFI",
    "installer/boot-files/EFI/XJ380/kernel.krl",
    "installer/system-index.pak",
};
} // namespace

int SourceMedia::raw(uint64_t offset, void *output, xjiso::Size bytes)
{
    if (offset > device_bytes || bytes > device_bytes - offset) return -EIO;
    return input(context, offset, output, bytes) ? 0 : -EIO;
}

int SourceMedia::fat_boot()
{
    int error = raw(volume_offset, sector, 512);
    if (error) return error;
    const char signature[] = "FAT32   ";
    for (unsigned i = 0; i < 8; ++i)
        if (sector[82 + i] != unsigned(signature[i])) return -EOPNOTSUPP;
    sector_bytes  = le16(sector + 11);
    unsigned unit = sector[13], reserved = le16(sector + 14);
    fats           = sector[16];
    uint64_t total = le32(sector + 32), sectors = le32(sector + 36);
    unsigned flags = le16(sector + 40);
    root           = le32(sector + 44);
    if (sector_bytes < 512 || sector_bytes > sizeof(sector) || (sector_bytes & (sector_bytes - 1)) || !unit ||
        unit > 128 || (unit & (unit - 1)) || !reserved || (fats != 1 && fats != 2) || !total || !sectors ||
        le16(sector + 17) || le16(sector + 22) || le16(sector + 42) || sector[510] != 0x55 || sector[511] != 0xaa ||
        total > volume_bytes / sector_bytes)
        return -EINVAL;
    uint64_t data  = uint64_t(reserved) + fats * sectors;
    uint64_t count = data < total ? (total - data) / unit : 0;
    if (count < 65525 || count >= 0x0ffffff0ULL || sectors * sector_bytes / 4 < count + 2 || root < 2 ||
        root > count + 1)
        return -EINVAL;
    clusters      = uint32_t(count);
    cluster_bytes = unit * sector_bytes;
    fat_start     = volume_offset + uint64_t(reserved) * sector_bytes;
    data_start    = volume_offset + data * sector_bytes;
    fat_bytes     = sectors * sector_bytes;
    mirrored      = !(flags & 0x80) && fats == 2;
    active_fat    = flags & 0x80 ? flags & 15 : 0;
    if (active_fat >= fats) return -EINVAL;
    cache_valid = false;
    return 0;
}

int SourceMedia::fat_next(uint32_t cluster, uint32_t &next)
{
    if (cluster < 2 || cluster > clusters + 1) return -EIO;
    uint64_t block = uint64_t(cluster) * 4 / sector_bytes;
    if (!cache_valid || cached_fat != block)
    {
        int error = raw(fat_start + active_fat * fat_bytes + block * sector_bytes, fat, sector_bytes);
        if (error) return error;
        if (mirrored)
        {
            error = raw(fat_start + fat_bytes + block * sector_bytes, mirror, sector_bytes);
            if (error) return error;
            for (unsigned i = 0; i < sector_bytes; ++i)
                if (fat[i] != mirror[i]) return -EIO;
        }
        cached_fat  = block;
        cache_valid = true;
    }
    next = le32(fat + uint64_t(cluster) * 4 % sector_bytes) & 0x0fffffffU;
    if (next >= 0x0ffffff8U) return 0;
    return next >= 2 && next <= clusters + 1 ? 0 : -EIO;
}

int SourceMedia::fat_find(uint32_t directory, const char *name, bool want_directory, uint32_t &first, uint64_t &bytes)
{
    char     long_name[261] = {};
    unsigned expected = 0, checksum = 0, entries = 0;
    bool     valid_long = false, found = false;
    while (directory >= 2 && directory <= clusters + 1)
    {
        uint64_t start = data_start + uint64_t(directory - 2) * cluster_bytes;
        for (unsigned block = 0; block < cluster_bytes; block += sector_bytes)
        {
            int error = raw(start + block, sector, sector_bytes);
            if (error) return error;
            for (unsigned at = 0; at < sector_bytes; at += 32)
            {
                if (++entries > 65536) return -E2BIG;
                const auto *entry = sector + at;
                if (!entry[0]) return found ? 0 : -ENOENT;
                if (entry[0] == 0xe5)
                {
                    valid_long = false;
                    expected   = 0;
                    continue;
                }
                if (entry[11] == 15)
                {
                    unsigned sequence = entry[0] & 31;
                    if (entry[0] & 0x40)
                    {
                        for (char &c : long_name)
                            c = 0;
                        expected   = sequence;
                        checksum   = entry[13];
                        valid_long = sequence && sequence <= 20;
                    }
                    if (!valid_long || !expected || sequence != expected || entry[13] != checksum || entry[12] ||
                        le16(entry + 26) || (entry[0] & 0xa0))
                    {
                        valid_long = false;
                        expected   = 0;
                        continue;
                    }
                    static const unsigned positions[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    for (unsigned i = 0; i < 13; ++i)
                    {
                        unsigned value = le16(entry + positions[i]);
                        if (value != 0xffff && value > 127) valid_long = false;
                        long_name[(sequence - 1) * 13 + i] = value == 0xffff ? 0 : char(value);
                    }
                    --expected;
                    continue;
                }
                unsigned actual = 0;
                for (unsigned i = 0; i < 11; ++i)
                    actual = ((actual & 1) << 7) + (actual >> 1) + entry[i], actual &= 255;
                char     short_name[13] = {};
                unsigned length         = 0;
                for (unsigned i = 0; i < 8 && entry[i] != ' '; ++i)
                    short_name[length++] = char(entry[i]);
                if (entry[8] != ' ')
                {
                    short_name[length++] = '.';
                    for (unsigned i = 8; i < 11 && entry[i] != ' '; ++i)
                        short_name[length++] = char(entry[i]);
                }
                bool match =
                    !(entry[11] & 8) && (same_name(short_name, name) ||
                                         (valid_long && !expected && actual == checksum && same_name(long_name, name)));
                valid_long = false;
                expected   = 0;
                if (!match) continue;
                if (found || bool(entry[11] & 16) != want_directory) return -EINVAL;
                first = (uint32_t(le16(entry + 20)) << 16) | le16(entry + 26);
                bytes = le32(entry + 28);
                if (first < 2 || first > clusters + 1 || (want_directory && bytes)) return -EIO;
                found = true;
            }
        }
        uint32_t next  = 0;
        int      error = fat_next(directory, next);
        if (error) return error;
        if (next >= 0x0ffffff8U) return found ? 0 : -ENOENT;
        directory = next;
    }
    return -EIO;
}

int SourceMedia::fat_lookup(const char *path, uint32_t &first, uint64_t &bytes)
{
    uint32_t directory = root;
    for (unsigned depth = 0; depth < 8; ++depth)
    {
        char     component[256] = {};
        unsigned length         = 0;
        while (*path && *path != '/')
        {
            if (length == 255) return -ENAMETOOLONG;
            component[length++] = *path++;
        }
        bool more  = *path == '/';
        int  error = fat_find(directory, component, more, first, bytes);
        if (error || !more) return error;
        ++path;
        directory = first;
    }
    return -E2BIG;
}

int SourceMedia::fat_map(unsigned file, uint32_t first, uint64_t bytes)
{
    auto &result     = fat_files[file];
    result           = {bytes, extent_count, 0};
    uint64_t logical = 0, steps = (bytes + cluster_bytes - 1) / cluster_bytes;
    if (!steps || steps > clusters) return -EIO;
    for (uint64_t step = 0; step < steps; ++step)
    {
        if (first < 2 || first > clusters + 1) return -EIO;
        uint64_t physical = data_start + uint64_t(first - 2) * cluster_bytes;
        uint64_t amount   = bytes - logical < cluster_bytes ? bytes - logical : cluster_bytes;
        if (result.count && extents[extent_count - 1].physical + extents[extent_count - 1].bytes == physical)
            extents[extent_count - 1].bytes += amount;
        else
        {
            if (extent_count == xjinstaller::StreamExtentMaximum) return -E2BIG;
            extents[extent_count++] = {logical, physical, amount};
            ++result.count;
        }
        logical        += amount;
        uint32_t next   = 0;
        int      error  = fat_next(first, next);
        if (error) return error;
        // A cycle cannot reach EOC at the exact expected length. The file-size
        // bound limits traversal even when a chain or directory is malicious.
        if (step + 1 == steps) return next >= 0x0ffffff8U ? 0 : -EIO;
        if (next >= 0x0ffffff8U) return -EIO;
        first = next;
    }
    return -EIO;
}

int SourceMedia::fat_disjoint()
{
    // Sort indices, retaining each file's logical-order map. Physical order is
    // deliberately unrestricted: an ordinary FAT copy may fragment backwards.
    for (unsigned i = 0; i < extent_count; ++i)
        order[i] = i;
    auto sift = [&](unsigned root_index, unsigned count) {
        while (root_index < count / 2)
        {
            unsigned child = root_index * 2 + 1;
            if (child + 1 < count && extents[order[child]].physical < extents[order[child + 1]].physical) ++child;
            if (extents[order[root_index]].physical >= extents[order[child]].physical) break;
            unsigned swap     = order[root_index];
            order[root_index] = order[child];
            order[child]      = swap;
            root_index        = child;
        }
    };
    for (unsigned i = extent_count / 2; i; --i)
        sift(i - 1, extent_count);
    for (unsigned i = extent_count; i > 1; --i)
    {
        unsigned swap = order[0];
        order[0]      = order[i - 1];
        order[i - 1]  = swap;
        sift(0, i - 1);
    }
    for (unsigned i = 1; i < extent_count; ++i)
    {
        const auto &prior = extents[order[i - 1]], &next = extents[order[i]];
        if (prior.bytes > next.physical - prior.physical) return -EIO;
    }
    return 0;
}

int SourceMedia::lookup(unsigned file, uint64_t expected)
{
    if (!expected) return -EINVAL;
    if (!optical)
    {
        uint32_t first = 0;
        uint64_t bytes = 0;
        int      error = fat_lookup(paths[file], first, bytes);
        if (error) return error;
        return bytes == expected ? fat_map(file, first, bytes) : -EINVAL;
    }
    auto        directory = iso.root();
    const char *path      = paths[file];
    for (unsigned depth = 0; depth < 8; ++depth)
    {
        char     component[256] = {};
        unsigned length         = 0;
        while (*path && *path != '/')
            component[length++] = *path++;
        xjiso::Entry entry;
        int          error = iso.lookup(directory, component, entry, work);
        if (error) return error;
        if (entry.inode.symlink || entry.inode.unsupported) return -EINVAL;
        if (!*path)
        {
            if (entry.inode.directory || entry.inode.size != expected) return -EINVAL;
            iso_files[file] = entry.inode;
            return 0;
        }
        if (!entry.inode.directory) return -ENOTDIR;
        ++path;
        directory = entry.inode;
    }
    return -E2BIG;
}

int SourceMedia::verify_catalog(const xjinstaller::StreamIndex &index)
{
    int error = lookup(3, index.catalog.size);
    if (error) return error;
    for (xjiso::Size at = 0; at < index.catalog.size;)
    {
        xjiso::Size amount = index.catalog.size - at;
        if (amount > sizeof(sector)) amount = sizeof(sector);
        error = read(3, at, sector, amount);
        if (error) return error;
        for (xjiso::Size i = 0; i < amount; ++i)
            if (sector[i] != index.catalog.data[at + i]) return -ESTALE;
        at += amount;
    }
    return 0;
}

int SourceMedia::open(xjiso::Reader reader, void *opaque, uint64_t bytes, uint64_t offset, uint64_t length,
                      const xjinstaller::StreamIndex &index)
{
    if (!reader || index.version != 3 || !index.catalog.data || !length || offset > bytes || length > bytes - offset)
        return -EINVAL;
    input         = reader;
    context       = opaque;
    device_bytes  = bytes;
    volume_offset = offset;
    volume_bytes  = length;
    optical       = false;
    extent_count  = 0;
    stage         = "fat-geometry";
    int error     = fat_boot();
    if (!error)
    {
        stage = "fat-payload";
        error = lookup(0, le64(index.extents.data + 16));
    }
    if (error == -EOPNOTSUPP || error == -ENOENT)
    {
        optical = true;
        stage   = "iso-mount";
        error   = iso.mount(reader, opaque, bytes, work);
        if (!error)
        {
            stage = "iso-payload";
            error = lookup(0, le64(index.extents.data + 16));
        }
    }
    if (error) return error;
    for (uint64_t i = 1; i < index.extent_count; ++i)
    {
        stage = "boot-files";
        error = lookup(unsigned(i), le64(index.extents.data + i * 24 + 16));
        if (error) return error;
    }
    stage = "catalog-match";
    error = verify_catalog(index);
    if (!error && !optical)
    {
        stage = "fat-crosslinks";
        error = fat_disjoint();
    }
    if (!error) stage = "ready";
    return error;
}

int SourceMedia::read(unsigned file, uint64_t offset, void *output, xjiso::Size bytes)
{
    if (file >= 4 || !output || !bytes) return -EINVAL;
    if (optical)
    {
        if (offset > iso_files[file].size || bytes > iso_files[file].size - offset) return -EINVAL;
        xjiso::Size done  = 0;
        int         error = iso.read(iso_files[file], offset, output, bytes, done, work);
        return error ? error : done == bytes ? 0 : -EIO;
    }
    const auto &source = fat_files[file];
    if (offset > source.bytes || bytes > source.bytes - offset) return -EINVAL;
    xjiso::Size done = 0;
    while (done < bytes)
    {
        unsigned low = source.first, high = low + source.count;
        while (low < high)
        {
            unsigned    middle = low + (high - low) / 2;
            const auto &extent = extents[middle];
            if (offset + done < extent.logical)
                high = middle;
            else if (offset + done - extent.logical >= extent.bytes)
                low = middle + 1;
            else
            {
                low = middle;
                break;
            }
        }
        if (low >= source.first + source.count) return -EIO;
        const auto &extent = extents[low];
        uint64_t    within = offset + done - extent.logical;
        xjiso::Size amount = xjiso::Size(extent.bytes - within);
        if (amount > bytes - done) amount = bytes - done;
        int error = raw(extent.physical + within, static_cast<uint8_t *>(output) + done, amount);
        if (error) return error;
        done += amount;
    }
    return 0;
}
} // namespace installer

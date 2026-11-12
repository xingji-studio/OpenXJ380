// SPDX-License-Identifier: Apache-2.0
// Pure target admission: the actual FatFs importer does not reinterpret links.
#include <installer/fat32.h>
#include <fs/fatfs/ff.h>

namespace xjinstaller
{
namespace
{
bool next(const xjpak2::Span &name, xjpak2::Size &at, uint32_t &code)
{
    if (at >= name.size) return false;
    uint8_t first = name.data[at++];
    if (first < 128) { code = first; return true; }
    unsigned count;
    if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 31; }
    else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 15; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 7; }
    else return false;
    if (count > name.size - at) return false;
    for (unsigned i = 0; i < count; ++i)
    {
        uint8_t value = name.data[at++];
        if ((value & 0xc0) != 0x80) return false;
        code = (code << 6) | (value & 63);
    }
    return !((count == 1 && code < 128) || (count == 2 && code < 2048) ||
             (count == 3 && code < 65536) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff));
}

bool name_hash(const xjpak2::Span &name, uint64_t &hash)
{
    if (!name.size || name.size > 511 || name.data[0] != '/') return false;
    hash = 14695981039346656037ULL;
    xjpak2::Size at = 0;
    unsigned component = 0;
    uint32_t previous = 0;
    while (at < name.size)
    {
        uint32_t code;
        if (!next(name, at, code) || code < 32 || code == 127 || code == '"' || code == '*' ||
            code == ':' || code == '<' || code == '>' || code == '?' || code == '\\' || code == '|') return false;
        if (code == '/')
        {
            if (previous == '.' || previous == ' ') return false;
            component = 0;
        }
        else if ((component += code >= 65536 ? 2 : 1) > 255) return false;
        previous = code;
        hash ^= ff_wtoupper(code);
        hash *= 1099511628211ULL;
    }
    return previous != '.' && previous != ' ';
}

bool same_name(const xjpak2::Span &a, const xjpak2::Span &b)
{
    xjpak2::Size x = 0, y = 0;
    while (x < a.size && y < b.size)
    {
        uint32_t ca, cb;
        if (!next(a, x, ca) || !next(b, y, cb) || ff_wtoupper(ca) != ff_wtoupper(cb)) return false;
    }
    return x == a.size && y == b.size;
}

void sift(Fat32Name *names, xjpak2::Size root, xjpak2::Size count)
{
    while (root < count / 2)
    {
        xjpak2::Size child = root * 2 + 1;
        if (child + 1 < count && names[child].hash < names[child + 1].hash) ++child;
        if (names[root].hash >= names[child].hash) return;
        Fat32Name value = names[root]; names[root] = names[child]; names[child] = value;
        root = child;
    }
}
} // namespace

int fat32_check_plan(const xjpak2::Plan &plan, Fat32Name *names, xjpak2::Size capacity,
                      uint64_t bytes, Fat32Requirements &output)
{
    output = {};
    if (!plan.validated || (!plan.legacy_v1 && !plan.reader.read) || !plan.count || !plan.records || !names ||
        plan.count > capacity || bytes < 64ULL * 1048576 || bytes % 512 || bytes / 512 >= 0x100000000ULL)
        return -22;
    Fat32Requirements result = {};
    result.cluster_bytes = fat32_cluster_bytes(bytes);
    // Upper FAT size uses one entry per sector, even when clusters are larger.
    // Each name reserves 4 KiB for LFN entries/directory cluster rounding.
    result.required_bytes = 32768 + 2 * (((bytes / 512 + 2) * 4 + 511) / 512) * 512;
    for (xjpak2::Size i = 0; i < plan.count; ++i)
    {
        const auto &record = plan.records[i];
        if ((record.kind != xjpak2::Kind::Regular && record.kind != xjpak2::Kind::Directory) ||
            record.payload.size > 0xffffffffULL || !name_hash(record.path, names[i].hash)) return -95;
        names[i].record = i;
        uint64_t cost = 4096;
        if (record.kind == xjpak2::Kind::Regular)
        {
            cost += (uint64_t(record.payload.size) + result.cluster_bytes - 1) / result.cluster_bytes *
                    result.cluster_bytes;
            result.data_bytes += record.payload.size;
        }
        if (cost > bytes || result.required_bytes > bytes - cost) return -28;
        result.required_bytes += cost;
    }
    for (xjpak2::Size start = plan.count / 2; start; --start) sift(names, start - 1, plan.count);
    for (xjpak2::Size end = plan.count; end > 1; --end)
    {
        Fat32Name value = names[0]; names[0] = names[end - 1]; names[end - 1] = value;
        sift(names, 0, end - 1);
    }
    for (xjpak2::Size i = 0; i < plan.count; ++i)
        for (xjpak2::Size j = i + 1; j < plan.count && names[i].hash == names[j].hash; ++j)
            if (same_name(plan.records[names[i].record].path, plan.records[names[j].record].path)) return -17;
    result.records = plan.count;
    output = result;
    return 0;
}
} // namespace xjinstaller

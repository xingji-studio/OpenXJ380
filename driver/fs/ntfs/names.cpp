// SPDX-License-Identifier: Apache-2.0
// Original Unicode lookup and directory-view code. The volume's own $UpCase
// table, not a host locale or a linked Unicode library, defines NTFS folding.
#include "native.h"
#include <errno.h>

namespace xjntfs {
namespace {

static bool exact(const char *left, const char *right)
{
    while (*left && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

static void sift(uint64_t *values, unsigned root, unsigned count)
{
    while (root < count / 2)
    {
        unsigned child = root * 2 + 1;
        if (child + 1 < count && values[child] < values[child + 1]) ++child;
        if (values[root] >= values[child]) break;
        uint64_t saved = values[root];
        values[root]   = values[child];
        values[child]  = saved;
        root           = child;
    }
}

static void sort_references(uint64_t *values, unsigned count)
{
    for (unsigned i = count / 2; i; --i)
        sift(values, i - 1, count);
    for (unsigned i = count; i > 1; --i)
    {
        uint64_t saved = values[0];
        values[0]      = values[i - 1];
        values[i - 1]  = saved;
        sift(values, 0, i - 1);
    }
}

struct Utf8Cursor
{
    const uint8_t *next;
    unsigned       consumed;
    uint16_t       pending;
};

static int next_unit(Utf8Cursor &cursor, uint16_t &unit, bool &end)
{
    end = false;
    if (cursor.pending)
    {
        unit           = cursor.pending;
        cursor.pending = 0;
        return 0;
    }
    if (cursor.consumed >= MaxName) return -ENAMETOOLONG;
    unsigned first = *cursor.next;
    if (!first)
    {
        end  = true;
        unit = 0;
        return 0;
    }
    unsigned following;
    uint32_t scalar, minimum;
    if (first < 0x80)
    {
        following = 0;
        scalar    = first;
        minimum   = 0;
    }
    else if (first >= 0xc2 && first <= 0xdf)
    {
        following = 1;
        scalar    = first & 31;
        minimum   = 0x80;
    }
    else if (first >= 0xe0 && first <= 0xef)
    {
        following = 2;
        scalar    = first & 15;
        minimum   = 0x800;
    }
    else if (first >= 0xf0 && first <= 0xf4)
    {
        following = 3;
        scalar    = first & 7;
        minimum   = 0x10000;
    }
    else
        return -EILSEQ;
    if (following >= MaxName - cursor.consumed) return -ENAMETOOLONG;
    ++cursor.next;
    ++cursor.consumed;
    for (unsigned i = 0; i < following; ++i)
    {
        unsigned byte = *cursor.next++;
        ++cursor.consumed;
        if ((byte & 0xc0) != 0x80) return -EILSEQ;
        scalar = (scalar << 6) | (byte & 63);
    }
    if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return -EILSEQ;
    if (scalar >= 0x10000)
    {
        scalar         -= 0x10000;
        unit            = uint16_t(0xd800 + (scalar >> 10));
        cursor.pending  = uint16_t(0xdc00 + (scalar & 1023));
    }
    else
        unit = uint16_t(scalar);
    return 0;
}

} // namespace

int Volume::names_equal(const char *left, const char *right, bool sensitive, bool &match) const
{
    match = false;
    if (!ready_) return -ENODEV;
    if (!left || !right) return -EINVAL;
    Utf8Cursor a         = {reinterpret_cast<const uint8_t *>(left), 0, 0};
    Utf8Cursor b         = {reinterpret_cast<const uint8_t *>(right), 0, 0};
    bool       identical = true;
    for (unsigned count = 0; count <= 255; ++count)
    {
        uint16_t l, r;
        bool     lend, rend;
        int      error = next_unit(a, l, lend);
        if (!error) error = next_unit(b, r, rend);
        if (error) return error;
        if (lend || rend)
        {
            // Validate the entire input even after a mismatch.
            if (lend && rend)
            {
                match = identical;
                return 0;
            }
            identical = false;
        }
        if ((sensitive ? l : upcase_[l]) != (sensitive ? r : upcase_[r])) identical = false;
    }
    return -ENAMETOOLONG;
}

int Volume::list_directory(const Inode &inode, DirectoryVisitor visitor, void *context, Workspace &work) const
{
    if (!visitor) return -EINVAL;
    struct Visibility
    {
        Workspace *work;
        unsigned   count;
    } visibility  = {&work, 0};
    auto remember = [](void *context, const DirectoryEntry &entry) -> int {
        auto &state = *static_cast<Visibility *>(context);
        if (entry.name_space != 2)
        {
            if (state.count == MaxEntries) return -EOPNOTSUPP;
            state.work->visible[state.count++] = entry.reference;
        }
        return 0;
    };
    int error = enumerate_directory(inode, remember, &visibility, work);
    if (error) return error;
    sort_references(work.visible, visibility.count);
    struct Filter
    {
        Visibility      *visibility;
        DirectoryVisitor visitor;
        void            *context;
    } filter  = {&visibility, visitor, context};
    auto emit = [](void *context, const DirectoryEntry &entry) -> int {
        auto &filter = *static_cast<Filter *>(context);
        if (entry.name_space == 2)
        {
            unsigned low = 0, high = filter.visibility->count;
            while (low < high)
            {
                unsigned middle = low + (high - low) / 2;
                if (filter.visibility->work->visible[middle] < entry.reference)
                    low = middle + 1;
                else
                    high = middle;
            }
            if (low < filter.visibility->count && filter.visibility->work->visible[low] == entry.reference) return 0;
        }
        return filter.visitor(filter.context, entry);
    };
    return enumerate_directory(inode, emit, &filter, work);
}

int Volume::lookup_entry(const Inode &directory, const char *name, DirectoryEntry &entry, Workspace &work) const
{
    if (!name || !*name) return -EINVAL;
    bool valid;
    int  error = names_equal(name, name, true, valid);
    if (error) return error;
    struct Search
    {
        const Volume   *volume;
        const char     *name;
        DirectoryEntry *result;
        unsigned        rank;
        bool            sensitive, ambiguous;
    } search     = {this, name, &entry, 0, directory.case_sensitive, false};
    auto visitor = [](void *context, const DirectoryEntry &candidate) -> int {
        auto *search = static_cast<Search *>(context);
        bool  match;
        // Namespace zero is also emitted by ordinary Windows CreateFileW.
        // Case policy belongs to the directory; exact matches still outrank
        // folded matches and ambiguous folded names remain refused.
        int   error = search->volume->names_equal(search->name, candidate.name, search->sensitive, match);
        if (error || !match) return error;
        unsigned rank = exact(search->name, candidate.name) ? 2 : 1;
        if (rank > search->rank)
        {
            *search->result   = candidate;
            search->rank      = rank;
            search->ambiguous = false;
        }
        else if (rank == search->rank &&
                 (candidate.reference != search->result->reference || !exact(candidate.name, search->result->name)))
            search->ambiguous = true;
        return 0;
    };
    error = enumerate_directory(directory, visitor, &search, work);
    if (error) return error;
    if (search.ambiguous) return -EIO;
    if (!search.rank) return -ENOENT;
    if (entry.name_space == 2)
    {
        struct Canonical
        {
            uint64_t        reference;
            DirectoryEntry *entry;
            unsigned        count;
        } canonical    = {entry.reference, &entry, 0};
        auto long_name = [](void *context, const DirectoryEntry &candidate) -> int {
            auto &state = *static_cast<Canonical *>(context);
            if (candidate.reference == state.reference && candidate.name_space != 2)
            {
                *state.entry = candidate;
                ++state.count;
            }
            return 0;
        };
        error = enumerate_directory(directory, long_name, &canonical, work);
        if (error) return error;
        if (canonical.count > 1) return -EOPNOTSUPP;
    }
    return 0;
}

} // namespace xjntfs

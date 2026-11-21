// SPDX-License-Identifier: Apache-2.0
#include "native.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool read_image(void *context, uint64_t offset, void *output, xjiso::Size length)
{
    return offset <= INT64_MAX && !fseeko(static_cast<FILE *>(context), off_t(offset), SEEK_SET) &&
           fread(output, 1, length, static_cast<FILE *>(context)) == length;
}

static int fail(const char *phase, int error)
{
    fprintf(stderr, "ERROR %s %d\n", phase, error);
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 3) return 3;
    FILE *image = fopen(argv[1], "rb");
    if (!image) return 3;
    fseeko(image, 0, SEEK_END);
    uint64_t         bytes = uint64_t(ftello(image));
    xjiso::Volume    volume;
    xjiso::Workspace work;
    int              error = volume.mount(read_image, image, bytes, work);
    if (error)
    {
        fclose(image);
        return fail("mount", error);
    }
    xjiso::Inode inode = volume.root();
    const char  *path  = argc > 3 ? argv[3] : "/";
    while (*path)
    {
        while (*path == '/')
            ++path;
        if (!*path) break;
        char     name[256];
        unsigned size = 0;
        while (*path && *path != '/')
        {
            if (size == 255)
            {
                fclose(image);
                return fail("path", -36);
            }
            name[size++] = *path++;
        }
        name[size] = 0;
        xjiso::Entry entry;
        error = volume.lookup(inode, name, entry, work);
        if (error)
        {
            fclose(image);
            return fail("lookup", error);
        }
        inode = entry.inode;
    }
    if (!strcmp(argv[2], "info"))
    {
        printf("%llu %llu %u\n", static_cast<unsigned long long>(volume.volume_bytes()),
               static_cast<unsigned long long>(inode.size), unsigned(volume.joliet()));
    }
    else if (!strcmp(argv[2], "list"))
    {
        xjiso::Cursor cursor;
        error = volume.cursor(inode, cursor);
        if (!error)
        {
            xjiso::Entry entry;
            while ((error = volume.next(cursor, entry, work)) == 1)
                puts(entry.name);
        }
    }
    else if (!strcmp(argv[2], "read"))
    {
        uint64_t      offset    = argc > 4 ? strtoull(argv[4], nullptr, 0) : 0;
        uint64_t      remaining = argc > 5 ? strtoull(argv[5], nullptr, 0) : inode.size;
        unsigned char buffer[65536];
        while (remaining)
        {
            size_t length = remaining < sizeof(buffer) ? size_t(remaining) : sizeof(buffer), completed = 0;
            error = volume.read(inode, offset, buffer, length, completed, work);
            if (error || !completed) break;
            if (fwrite(buffer, 1, completed, stdout) != completed) return 3;
            offset    += completed;
            remaining -= completed;
        }
    }
    else
        error = -22;
    fclose(image);
    return error < 0 ? fail(argv[2], error) : 0;
}

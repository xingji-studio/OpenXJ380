#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

// First-party, read-only on-disk parser. There is deliberately no write callback.
// The caller owns block-device access policy and serializes device lifetimes.
namespace xjext4 {

using Size = __SIZE_TYPE__;
using Reader = bool (*)(void *context, uint64_t offset, void *output, Size length);
using DirectoryVisitor = int (*)(void *context, const char *name, uint32_t inode, uint8_t type);
using BlockVisitor = int (*)(void *context, uint64_t block);
// The raw visitor exposes namespace indices without inventing POSIX names for
// private/unrecognized indices. Its name/value views live only during the call.
using XattrVisitor = int (*)(void *context, uint8_t index, const char *name, const void *value, Size length);

struct Inode
{
    uint32_t number;
    uint32_t uid;
    uint32_t gid;
    uint32_t flags;
    uint32_t generation;
    uint16_t mode;
    uint16_t links;
    uint64_t size;
    uint64_t blocks_512;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t atime_nsec;
    uint32_t mtime_nsec;
    uint32_t ctime_nsec;
    uint64_t xattr_block;
    uint8_t block_map[60];
};

class WriteSession;

class Volume
{
public:
    const uint8_t *uuid() const { return uuid_; }
    // Safe for a zero-initialized trivial Volume embedded in a calloc'ed mount.
    void reset();
    int mount(Reader reader, void *context, uint64_t bytes);
    int read_inode(uint32_t number, Inode &inode) const;
    int read(const Inode &inode, uint64_t offset, void *output, Size length, Size &completed) const;
    // Visitor: zero continues; positive stops successfully; negative is returned.
    int list_directory(const Inode &inode, DirectoryVisitor visitor, void *context) const;
    int lookup(const Inode &directory, const char *name, Inode &inode) const;
    int get_xattr(uint32_t number, const char *name, void *value, Size capacity, Size &required) const;
    int list_xattr(uint32_t number, char *names, Size capacity, Size &required) const;
    int get_xattr_raw(uint32_t number, uint8_t index, const char *name, void *value,
                      Size capacity, Size &required) const;
    int list_xattr_raw(uint32_t number, XattrVisitor visitor, void *context) const;

    uint32_t block_size() const { return block_size_; }
    uint64_t blocks_count() const { return blocks_; }
    uint64_t free_blocks() const { return free_blocks_; }
    uint32_t inodes_count() const { return inodes_; }
    uint32_t free_inodes() const { return free_inodes_; }

private:
    friend class WriteSession;
    // Bootstrap is private to recovery: it reads immutable journal locations
    // through possibly torn allocation counters/checksums, never user data.
    int mount_internal(Reader reader, void *context, uint64_t bytes, bool recovery, bool bootstrap = false);
    int read_group(uint32_t group, uint8_t *descriptor) const;

    Reader reader_;
    void *context_;
    uint64_t bytes_;
    uint64_t blocks_;
    uint64_t free_blocks_;
    uint32_t block_size_;
    uint32_t inodes_;
    uint32_t free_inodes_;
    uint32_t blocks_per_group_;
    uint32_t inodes_per_group_;
    uint32_t first_data_block_;
    uint32_t groups_;
    uint32_t incompat_;
    uint32_t readonly_;
    uint32_t checksum_seed_;
    uint16_t inode_size_;
    uint16_t descriptor_size_;
    uint8_t uuid_[16];
    bool ready_;
    bool bootstrap_;

    int fetch(uint64_t offset, void *output, Size length) const;
    int read_raw_inode(uint32_t number, uint8_t *raw, uint64_t *location = nullptr) const;
    int orphan_member(uint32_t number, bool &member) const;
    int map_block(const Inode &inode, uint32_t logical, uint64_t &physical, bool &unwritten,
                  BlockVisitor metadata = nullptr, void *context = nullptr) const;
    int map_extents(const Inode &inode, uint32_t logical, uint64_t &physical, bool &unwritten,
                    BlockVisitor metadata = nullptr, void *context = nullptr) const;
    uint32_t inode_seed(const Inode &inode) const;
    int check_directory_block(const Inode &inode, uint8_t *block, uint64_t logical) const;
};

} // namespace xjext4

#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

namespace xjext4 {

using DirectorySize = __SIZE_TYPE__;

struct DirectoryHash
{
    uint32_t major;
    uint32_t minor;
};

// Version 0/1/2 selects signed-byte legacy/half-MD4/TEA; 3/4/5 selects
// unsigned-byte variants. This is an independent arithmetic implementation.
int directory_hash(uint8_t version, const uint32_t seed[4], const char *name,
                   DirectorySize length, DirectoryHash &result);

struct DirectoryIndexIO
{
    void *context;
    int (*read)(void *, uint32_t logical, uint8_t *output);
    int (*edit)(void *, uint32_t logical, uint8_t *&output);
    int (*append)(void *, uint32_t &logical, uint8_t *&output);
    uint32_t block_size;
    uint32_t block_count;
    uint32_t inode;
    uint32_t inode_count;
    uint32_t inode_seed;
    uint32_t hash_seed[4];
    uint8_t default_hash_version; // On-disk algorithm 0..2; signedness is separate.
    bool unsigned_hash;
    bool metadata_checksum;
    bool file_type;
    bool large_directory;
    // Root promotion to indirect_levels=2 must atomically enable LARGEDIR in
    // the same filesystem transaction. A read-only adapter leaves this null.
    int (*enable_large_directory)(void *);
};

// Caller-owned heap workspace, reused after each namespace operation. The
// visited set covers the native writer's maximum 128 * 32768 logical blocks.
// Neither this workspace nor the owning WriteWorkspace belongs on a stack.
struct DirectoryIndexWorkspace
{
    struct LeafEntry
    {
        uint32_t hash;
        uint32_t minor;
        uint32_t inode;
        uint16_t offset;
        uint16_t length;
        uint8_t type;
    };
    struct IndexEntry
    {
        uint32_t hash;
        uint32_t logical;
    };
    uint8_t visited[524288];
    uint8_t nodes[3][4096];
    uint8_t leaf[4096];
    LeafEntry entries[342];
    IndexEntry indices[512];
};

struct DirectoryIndexPosition
{
    uint32_t logical;
    uint32_t inode;
    uint16_t offset;
    uint16_t previous;
    uint8_t type;
};

int directory_leaf_check(const DirectoryIndexIO &io, const uint8_t *block);
void directory_leaf_seal(const DirectoryIndexIO &io, uint8_t *block);
int directory_node_check(const DirectoryIndexIO &io, bool root, const uint8_t *block);
void directory_node_seal(const DirectoryIndexIO &io, bool root, uint8_t *block);

// Index nodes and leaves retain their on-disk roles, including empty leaves
// after deletion. Root promotion and every split use the caller's transaction
// callbacks; this layer never checkpoints or writes a device directly.
// Mutation errors can leave staged images: the caller must abort its overlay.
class IndexedDirectory
{
public:
    IndexedDirectory(const DirectoryIndexIO &io, DirectoryIndexWorkspace &workspace);
    int validate();
    int find(const char *name, DirectorySize length, DirectoryIndexPosition &position);
    int insert(const char *name, DirectorySize length, uint32_t inode, uint8_t type);
    int remove(const DirectoryIndexPosition &position);
    int empty(uint32_t expected_parent);
    int convert_and_insert(const char *name, DirectorySize length, uint32_t inode, uint8_t type);

private:
    struct Frame
    {
        uint32_t logical;
        uint16_t slot;
        uint16_t count;
        uint32_t lower;
        uint64_t upper;
    };
    DirectoryIndexIO io_;
    DirectoryIndexWorkspace &workspace_;
    Frame path_[3];
    uint8_t version_;
    uint8_t stored_version_;
    uint8_t depth_;
    bool ready_;

    int claim(uint32_t logical);
    int walk_index(unsigned level, uint32_t logical, unsigned remaining, uint32_t lower, uint64_t upper);
    int check_leaf(uint32_t logical, uint32_t lower, uint64_t upper);
    int read_node(unsigned level, uint32_t logical);
    int probe(uint32_t hash, uint32_t &leaf);
    int next_leaf(uint32_t &leaf, uint32_t &boundary);
    int append(uint32_t &logical, uint8_t *&output);
    int collect_leaf(const uint8_t *block, bool skip_dots, unsigned &count, uint32_t &bytes);
    int add_entry(const char *name, DirectorySize length, uint32_t inode, uint8_t type,
                  unsigned &count, uint32_t &bytes);
    void sort_entries(unsigned count);
    void pack_leaf(uint8_t *output, unsigned first, unsigned end, const char *name);
    int split_leaf(uint32_t logical, const char *name, DirectorySize length, uint32_t inode, uint8_t type,
                   uint32_t &new_logical, uint32_t &boundary);
    int insert_index(uint32_t logical, uint32_t boundary);
    void pack_node(uint8_t *output, bool root, unsigned first, unsigned end);
};

} // namespace xjext4

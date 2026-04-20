// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "index.h"

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
// Returns 0 on success, -1 on error.


// Forward declaration for helper function
static int write_tree_level(IndexEntry *entries, int count, const char *prefix, ObjectID *id_out);


// Load the index and delegate to recursive helper
int tree_from_index(ObjectID *id_out) {
    // Step 1: Load the index from disk (allocate on heap to avoid stack overflow)
    Index *index = malloc(sizeof(Index));
    if (!index) {
        fprintf(stderr, "error: out of memory\n");
        return -1;
    }
    
    if (index_load(index) != 0) {
        free(index);
        fprintf(stderr, "error: failed to load index\n");
        return -1;
    }
    
    if (index->count == 0) {
        free(index);
        fprintf(stderr, "error: index is empty\n");
        return -1;
    }

    // Step 2: Build tree starting at root level (empty prefix)
    int rc = write_tree_level(index->entries, index->count, "", id_out);
    free(index);
    return rc;
}

// Helper function for recursive tree building (incomplete - will finish in next commit)
static int write_tree_level(IndexEntry *entries, int count, const char *prefix, ObjectID *id_out) {
    // Initialize tree at this level
    Tree tree;
    tree.count = 0;
    
    // Step 1: Get prefix length for relative path calculation
    size_t prefix_len = strlen(prefix);
    int i = 0;
    
    // Step 2: Iterate through entries looking for entries that start with this prefix
    while (i < count && tree.count < MAX_TREE_ENTRIES) {
        const char *path = entries[i].path;
        
        // Skip entries that don't start with our prefix
        if (strncmp(path, prefix, prefix_len) != 0) {
            i++;
            continue;
        }
        
        // Step 3: Get the relative path after the prefix
        const char *rel_path = path + prefix_len;
        
        // Step 4: Check if this is a file at this level or in a subdirectory
        const char *slash = strchr(rel_path, '/');
        
        if (!slash) {
            // This is a FILE at this level (no '/' found)
            // Add it directly to the tree
            TreeEntry *entry = &tree.entries[tree.count];
            entry->mode = entries[i].mode;
            entry->hash = entries[i].hash;
            strncpy(entry->name, rel_path, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
            tree.count++;
            i++;
        } else {
            // This is a SUBDIRECTORY
            // Extract the directory name (part before the slash)
            size_t dir_name_len = slash - rel_path;
            char dir_name[256];
            memcpy(dir_name, rel_path, dir_name_len);
            dir_name[dir_name_len] = '\0';
            
            // Build the new prefix for the subdirectory
            char new_prefix[512];
            snprintf(new_prefix, sizeof(new_prefix), "%s%s/", prefix, dir_name);
            
            // Count how many entries belong to this subdirectory
            size_t new_prefix_len = strlen(new_prefix);
            while (i < count && strncmp(entries[i].path, new_prefix, new_prefix_len) == 0) {
                i++;
            }
                        
            // Recursively build tree for this subdirectory
            ObjectID subtree_id = {0};
            if (write_tree_level(entries, count, new_prefix, &subtree_id) != 0) {
                return -1;
            }
            
            // Add directory entry to current tree
            TreeEntry *entry = &tree.entries[tree.count];
            entry->mode = MODE_DIR;  // Mode for directory
            entry->hash = subtree_id;
            snprintf(entry->name, sizeof(entry->name), "%s", dir_name);
            entry->name[sizeof(entry->name) - 1] = '\0';
            tree.count++;
            
            // Skip past all entries in this subdirectory
            
        }
    }
    
    // Step 5: Serialize the tree to binary format
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) {
        return -1;
    }
    
    // Step 6: Write the tree object to the object store
    // (Note: object_write() will be implemented in Phase 1 - already done)
    extern int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    
    return rc;
}
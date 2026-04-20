// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    // Step 1: Initialize index as empty
    index->count = 0;
    
    // Step 2: Try to open the index file
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        // File doesn't exist - this is OK for first commit
        return 0;
    }
    
    // Step 3: Parse each line from the index file
    char hex[HASH_HEX_SIZE + 1];
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *entry = &index->entries[index->count];
        
        // Step 4: Parse line format:
        // <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
        int mode;
        uint64_t mtime;
        uint32_t size;
        
        int rc = fscanf(f, "%o %64s %lu %u %511s\n",
                       &mode, hex, &mtime, &size, entry->path);
        
        if (rc != 5) {
            // End of file or parse error
            break;
        }
        
        // Step 5: Store parsed values in index entry
        entry->mode = mode;
        entry->mtime_sec = mtime;
        entry->size = size;
        
        // Step 6: Convert hex hash to binary ObjectID
        if (hex_to_hash(hex, &entry->hash) != 0) {
            fclose(f);
            return -1;
        }
        
        index->count++;
    }
    
    // Step 7: Close file and return success
    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically.
//
// HINTS - Useful functions and syscalls:
//   - qsort                            : sorting the entries array by path
//   - fopen (with "w"), fprintf        : writing to the temporary file
//   - hash_to_hex                      : converting ObjectID for text output
//   - fflush, fileno, fsync, fclose    : flushing userspace buffers and syncing to disk
//   - rename                           : atomically moving the temp file over the old index
//
// Returns 0 on success, -1 on error.
// Helper function for qsort to sort index entries by path
static int compare_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Save the index to .pes/index atomically
int index_save(const Index *index) {
    // Step 1: Create a mutable copy to sort
    Index sorted = *index;
    
    // Step 2: Sort entries by path (deterministic order)
    qsort(sorted.entries, sorted.count, sizeof(IndexEntry), compare_entries);
    
    // Step 3: Open temporary file for writing
    FILE *f = fopen(INDEX_FILE ".tmp", "w");
    if (!f) return -1;
    
    // Step 4: Write each entry to temporary file
    for (int i = 0; i < sorted.count; i++) {
        const IndexEntry *entry = &sorted.entries[i];
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&entry->hash, hex);
        
        // Format: <mode-octal> <hex> <mtime> <size> <path>
        int rc = fprintf(f, "%o %s %lu %u %s\n",
                        entry->mode, hex,
                        (unsigned long)entry->mtime_sec,
                        entry->size, entry->path);
        
        if (rc < 0) {
            fclose(f);
            unlink(INDEX_FILE ".tmp");
            return -1;
        }
    }
    
    // Step 5: Flush to userspace buffer
    if (fflush(f) != 0) {
        fclose(f);
        unlink(INDEX_FILE ".tmp");
        return -1;
    }
    
    // Step 6: Sync to disk
    if (fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(INDEX_FILE ".tmp");
        return -1;
    }
    
    fclose(f);
    
    // Step 7: Atomically move temp file to final location
    if (rename(INDEX_FILE ".tmp", INDEX_FILE) != 0) {
        unlink(INDEX_FILE ".tmp");
        return -1;
    }
    
    return 0;
}

// Stage a file for the next commit.
//
// HINTS - Useful functions and syscalls:
//   - fopen, fread, fclose             : reading the target file's contents
//   - object_write                     : saving the contents as OBJ_BLOB
//   - stat / lstat                     : getting file metadata (size, mtime, mode)
//   - index_find                       : checking if the file is already staged
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    // Step 1: Open the file for reading
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: could not open '%s'\n", path);
        return -1;
    }
    
    // Step 2: Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // Step 3: Allocate buffer for file contents
    void *data = malloc(file_size);
    if (!data) {
        fclose(f);
        return -1;
    }
    
    // Step 4: Read entire file into buffer
    size_t read_bytes = fread(data, 1, file_size, f);
    fclose(f);
    
    if (read_bytes != (size_t)file_size) {
        free(data);
        return -1;
    }
    // Step 5: Write file contents as blob object to store
    extern int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, data, file_size, &blob_id) != 0) {
        free(data);
        return -1;
    }
    free(data);
    
    // Step 6: Get file metadata (for index entry)
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    
    // Step 7: Determine file mode (executable or regular)
    uint32_t mode = 0100644;  // Regular file
    if (st.st_mode & 0111) {
        mode = 0100755;  // Executable file
    }
    
    // (Index entry update will be in next commit)
    (void)mode; (void)blob_id;
    return -1;
}
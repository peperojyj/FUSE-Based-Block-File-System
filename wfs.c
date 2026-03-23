#include "wfs.h"
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

char *disk;
struct wfs_sb *super_block;
int wfs_error;

struct wfs_inode *get_child_inode(const char *path) {
    struct wfs_inode *curr_inode = (struct wfs_inode *)(disk + super_block->i_blocks_ptr);
    char *temp_path = strdup(path);

    if (strcmp(temp_path, "/") == 0) {
        free(temp_path);
        return curr_inode;
    }

    char *token = strtok(temp_path, "/");
    while (token != NULL) {
        if ((curr_inode->mode & __S_IFMT) != __S_IFDIR) { // if not directory
            free(temp_path);
            wfs_error = -ENOENT;
            return NULL;
        }

        int found = 0;
        
        int num_blocks = curr_inode->size / BLOCK_SIZE;
        if (curr_inode->size % BLOCK_SIZE != 0) {
            num_blocks++;
        }

        for (int i = 0; i < num_blocks; i++) {
            struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + curr_inode->blocks[i]);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                if (strcmp(dentry[j].name, token) == 0) {
                    curr_inode = (struct wfs_inode *) (disk + super_block->i_blocks_ptr + dentry[j].num * BLOCK_SIZE);
                    found = 1;
                    break;
                }
            }
        }

        if (!found) {
            wfs_error = -ENOENT;
            free(temp_path);
            return NULL;
        }
        token = strtok(NULL, "/");
    }
    free(temp_path);
    return curr_inode;
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(struct stat));
    struct wfs_inode *dest_inode = get_child_inode(path);
    if (dest_inode == NULL) {
        return wfs_error;
    }

    stbuf->st_mode = dest_inode->mode;
    stbuf->st_uid = dest_inode->uid;
    stbuf->st_gid = dest_inode->gid;
    stbuf->st_size = dest_inode->size;
    stbuf->st_nlink = dest_inode->nlinks;
    stbuf->st_atime = dest_inode->atim;
    stbuf->st_mtime = dest_inode->mtim;
    stbuf->st_ctime = dest_inode->ctim;
    return 0;
}

off_t get_free_block_bit() {
    unsigned int *data_bitmap = (unsigned int *)(disk + super_block->d_bitmap_ptr);
    int num_data_blocks = super_block->num_data_blocks;

    for (int block_num = 0; block_num < num_data_blocks; block_num++) {
        int index = block_num / 32;
        int bit = block_num % 32;

        if (!(data_bitmap[index] & (1U << bit))) {  // Check if the bit is not set
            data_bitmap[index] |= (1U << bit); 
            return super_block->d_blocks_ptr + block_num * BLOCK_SIZE;
        }
    }
    wfs_error = -ENOSPC;
    return -1;  // Return -1 if no block is available
}

void remove_data_block(off_t block) {
    int block_num = (block - super_block->d_blocks_ptr) / BLOCK_SIZE;
    unsigned int *data_bitmap = (unsigned int *)(disk + super_block->d_bitmap_ptr);

    int index = block_num / 32;
    int bit = block_num % 32;   

    data_bitmap[index] &= ~(1U << bit);

    memset(disk + block, 0, sizeof(BLOCK_SIZE));
}

int resize(int size, struct wfs_inode *inode) {
    int num_blocks = size / BLOCK_SIZE;
    if (size % BLOCK_SIZE != 0) {
        num_blocks++;
    }

    int curr_inode_blocks = inode->size / BLOCK_SIZE;
    if (inode->size % BLOCK_SIZE != 0) {
        curr_inode_blocks++;
    }

    if (num_blocks == curr_inode_blocks) {
        return 0;
    }

    if (num_blocks < curr_inode_blocks) { // less than
        for (int i = num_blocks; i < curr_inode_blocks; i++){
            remove_data_block(inode->blocks[i]);
            inode->blocks[i] = 0;
        }
    } else { // greater than
        if (num_blocks > D_BLOCK) { // indrect block!
            if (inode->blocks[D_BLOCK] == 0) {
                off_t block = get_free_block_bit();
                if (block == -1) {
                    wfs_error = -ENOSPC;
                    return -1;
                }
                inode->blocks[D_BLOCK] = block;
            }
        }

        for (int i = curr_inode_blocks; i < num_blocks; i++) {
            off_t block = get_free_block_bit();
            if (block == -1) {
                wfs_error = -ENOSPC;
                return -1;
            }
            if (i < D_BLOCK) { // alloc direct blocks
                inode->blocks[i] = block;
            } else {  // alloc indirect blocks
                char *indirect_block = disk + inode->blocks[D_BLOCK];
                off_t *indirect_block_offset = (off_t *)indirect_block;
                indirect_block_offset[i - D_BLOCK] = block;
            }
        }
    }
    inode->size = size;
    return 0;
}

int update_parent_dentry(struct wfs_inode *parent_inode, const char *name, int index) {
    for (int i = 0; i < D_BLOCK; i++) {
        if (parent_inode->blocks[i] == 0) {
            resize(parent_inode->size + BLOCK_SIZE, parent_inode);  // Ensure there's space
        }

        struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + parent_inode->blocks[i]);
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
            if (dentry[j].num == 0) { 
                dentry[j].num = index;
                strcpy(dentry[j].name, name);
                return 1;  // Success
            }
        }
    }
    wfs_error = -ENOSPC;
    return 0;  // No space found
}

int remove_parent_dentry(struct wfs_inode *parent_inode, const char *name, int index) {
    for (int i = 0; i < D_BLOCK; i++) {
        if (parent_inode->blocks[i] != 0) {
            struct wfs_dentry *dentry = (struct wfs_dentry*)(disk + parent_inode->blocks[i]);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                if (dentry[j].num == index) {
                    dentry[j].num = 0;
                    strcpy(dentry[j].name, "");
                    resize(parent_inode->size - sizeof(struct wfs_dentry), parent_inode);
                    return 1; // success
                }
            }
        }
    }
    wfs_error = -ENOENT;
    return 0;
}

struct wfs_inode *get_parent_inode(const char *path) {
    struct wfs_inode *curr_inode = (struct wfs_inode *)(disk + super_block->i_blocks_ptr);
    char *temp_path = strdup(path);
    char *token = strtok(temp_path, "/"); 
    char *next_token;

    while (token != NULL) {
        next_token = strtok(NULL, "/");

        if (next_token == NULL) {
            free(temp_path);
            return curr_inode;
        }

        if ((curr_inode->mode & __S_IFMT) != __S_IFDIR) {
            free(temp_path);
            wfs_error = -ENOENT;
            return NULL;  // Not a directory
        }

        int found = 0;
        for (int i = 0; i < (curr_inode->size + BLOCK_SIZE - 1) / BLOCK_SIZE; i++) {
            struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + curr_inode->blocks[i]);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                if (strcmp(dentry[j].name, token) == 0) {
                    curr_inode = (struct wfs_inode *)(disk + super_block->i_blocks_ptr + dentry[j].num * BLOCK_SIZE);
                    found = 1;
                    break;
                }
            }
            if (found) {
                break;
            }
        }

        if (!found) {
            free(temp_path);
            wfs_error = -ENOENT;
            return NULL;  // Path component not found
        }

        token = next_token;
    }

    free(temp_path);
    wfs_error = -ENOENT;
    return NULL;  // Path not fully resolved
}

struct wfs_inode *create_inode(int size, const char *path) {
    struct wfs_inode *curr_inode = get_parent_inode(path);
    if (curr_inode == NULL){
        wfs_error = -ENOENT;
        return NULL;
    }

    char *token = strrchr(path, '/');
    if (token == NULL) {
        wfs_error = -ENOENT;
        return NULL;
    }
    token++;
    
    int index = -1;
    unsigned char *inode_bitmap = (unsigned char *)(disk + super_block->i_bitmap_ptr);
    int num_inodes = super_block->num_inodes;

    for (int i = 1; i < num_inodes; ++i) {
        int index_byte = i / 8;
        int index_bit = i % 8;
        if (!((inode_bitmap[index_byte] >> index_bit) & 1)) {
            index = i;
            inode_bitmap[index_byte] |= (1 << index_bit);
            break;
        }
    }

    if (index == -1) {
        wfs_error = -ENOSPC;
        return NULL;
    }

    if (!update_parent_dentry(curr_inode, token, index)) {
        wfs_error = -ENOSPC;
        return NULL;  // Failed to add directory entry
    }

    struct wfs_inode *new_inode = (struct wfs_inode *)(disk + super_block->i_blocks_ptr + index * BLOCK_SIZE);
    new_inode->num = index;
    new_inode->size = size;
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->nlinks = 1;
    new_inode->atim = time(NULL);
    new_inode->mtim = time(NULL);
    new_inode->ctim = time(NULL);
    
    // Allocate blocks
    int num_blocks = size / BLOCK_SIZE;
    if (size % BLOCK_SIZE != 0) {
        num_blocks++;
    }

    for (int i = 0; i < num_blocks; i++) {
        off_t block = get_free_block_bit();
        if (block == -1) {
            wfs_error = -ENOSPC;
            return NULL;
        }
        new_inode->blocks[i] = block;
    }
    return new_inode;
}

static int wfs_mkdir(const char *path, mode_t mode) {
    struct wfs_inode *inode = get_child_inode(path);
    if (inode != NULL) { // already exitst
        return wfs_error;
    }

    inode = create_inode(0, path);
    if (inode == NULL) {
        return wfs_error;
    }

    inode->mode = __S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR;

    return 0;
}

static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    struct wfs_inode *inode = get_child_inode(path);
    if (inode != NULL) { // already exist
        return wfs_error;
    }

    inode = create_inode(0, path);
    if (inode == NULL) {
        return wfs_error;
    }

    inode->mode = mode;

    return 0;
}

void clear_inode(struct wfs_inode *inode) {
    if (inode == NULL) {
        return;
    }

    unsigned int *inode_bitmap = (unsigned int *)(disk + super_block->i_bitmap_ptr);
    int inode_num = inode->num;
    int index = inode_num / 32;
    int bit = inode_num % 32;

    inode_bitmap[index] &= ~(1U << bit);

    memset(inode, 0, sizeof(struct wfs_inode));
}  

static int wfs_unlink(const char* path) {
    struct wfs_inode *inode = get_child_inode(path);
    if (inode == NULL) {
        return wfs_error;
    }

    if ((inode->mode & __S_IFMT) == __S_IFDIR) {
        return -EISDIR;
    }

    struct wfs_inode *p_inode = get_parent_inode(path);
    if (p_inode == NULL) {
        return wfs_error;
    }


    for(int i = 0; i < N_BLOCKS; i++) {
        if (inode->blocks[i] != 0) {
            remove_data_block(inode->blocks[i]);
        }
    }
    char *child_name = strrchr(path, '/');
    if (child_name == NULL) {
        wfs_error = -ENOENT;
        return wfs_error;
    } 
    child_name++;
    if (!remove_parent_dentry(p_inode,child_name, inode->num)) {
        return wfs_error;
    }
    clear_inode(inode);

    return 0;
}

int remove_dir(struct wfs_inode *inode, const char *path) {
    struct wfs_inode *parent_inode = get_parent_inode(path);

    if (parent_inode == NULL) {
        wfs_error = -ENOENT;
        return 0; // Parent inode not found, cannot proceed
    }

    char *child_name = strrchr(path, '/');
    if (child_name == NULL) {
        wfs_error = -ENOENT;
        return 0;
    } 
    child_name++;

    int inode_blocks = inode->size / BLOCK_SIZE;
    if (inode->size % BLOCK_SIZE != 0) {
        inode_blocks++;
    }

    if (!remove_parent_dentry(parent_inode, child_name, inode->num)) {
        wfs_error = -ENOENT;
        return 0;
    }
    
    for (int i = 0; i < inode_blocks; i++) {
        if (inode->blocks[i] != 0) {
            remove_data_block(inode->blocks[i]);
            inode->blocks[i] = 0;
        }
    }

    clear_inode(inode);

    return 1;
}

static int wfs_rmdir(const char *path) {
    struct wfs_inode *inode = get_child_inode(path);
    if (inode == NULL) {
        return wfs_error;
    }

    if ((inode->mode & __S_IFMT) != __S_IFDIR) { 
        wfs_error = -ENOENT;
        return wfs_error;
    }

    for (int i = 0; i < inode->size / BLOCK_SIZE; i++) {
        struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + inode->blocks[i]);
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
            if (dentry[j].num != 0) {
                return -1; // Directory not empty
            }
        }
    }

    if (!remove_dir(inode,path)) {
        return wfs_error;
    }
    return 0;
}


static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    struct wfs_inode *inode = get_child_inode(path);
    if (inode == NULL){
        return wfs_error;
    }

    if (offset >= inode->size) { // beyond or at
        return 0; 
    }

    int start_block = offset / BLOCK_SIZE;
    int end_block = (offset + size) / BLOCK_SIZE;
    if ((offset + size) % BLOCK_SIZE != 0) {
        end_block++;
    }

    int start_index = offset % BLOCK_SIZE;

    int process = 0;
    int remaining = size;

    int curr_block_index = start_block;

    int inode_size = inode->size / BLOCK_SIZE;
    if (inode->size % BLOCK_SIZE != 0){
        inode_size++;
    }

    while (curr_block_index <= end_block && curr_block_index < inode_size)  {
        int start_pt;
        if (curr_block_index == start_block) {
            start_pt = start_index;
        } else {
            start_pt = 0;
        }

        int bytes_in_block = BLOCK_SIZE - start_pt;
        int bytes_to_copy;

        if (remaining < bytes_in_block) {
            bytes_to_copy = remaining;
        } else {
            bytes_to_copy = bytes_in_block;
        }

        char *block_ptr = NULL;

        if (curr_block_index < D_BLOCK) {
            block_ptr = disk + inode->blocks[curr_block_index]; // direct block pointer
        } else {
            char *indirect_block  = disk + inode->blocks[D_BLOCK];
            off_t *indirect_block_ptr = (off_t*) indirect_block;

            int indirect_index = curr_block_index - D_BLOCK;

            if (indirect_index < (BLOCK_SIZE / sizeof(off_t))) {
                block_ptr = disk + indirect_block_ptr[indirect_index];
            } else {
                break;
            }
        }

        if (block_ptr != NULL) {
            memcpy(buf + process, block_ptr + start_pt, bytes_to_copy);
            process += bytes_to_copy;
            remaining -= bytes_to_copy;
        }

        curr_block_index++;
    }

    return process;
}


static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct wfs_inode *inode = get_child_inode(path);
    if (inode == NULL) {
        return wfs_error;
    }

    if ((offset + size) > inode->size) {
        if (resize(offset + size, inode) != 0) {
            return wfs_error;
        }
    }

    int start_block = offset / BLOCK_SIZE;
    int end_block = (offset + size) / BLOCK_SIZE;
    if ((offset + size) % BLOCK_SIZE != 0) {
        end_block++;
    }

    int start_index = offset % BLOCK_SIZE;

    int process = 0;
    int remaining = size;

    int curr_block_index = start_block;

    int inode_size = inode->size / BLOCK_SIZE;
    if (inode->size % BLOCK_SIZE != 0){
        inode_size++;
    }

    while (curr_block_index <= end_block && curr_block_index < inode_size) {
        int start_pt;
        if (curr_block_index == start_block) {
            start_pt = start_index;
        } else {
            start_pt = 0;
        }

        int bytes_in_block = BLOCK_SIZE - start_pt;
        int bytes_to_copy;

        if (remaining < bytes_in_block) {
            bytes_to_copy = remaining;
        } else {
            bytes_to_copy = bytes_in_block;
        }

        char *block_ptr = NULL;

        if (curr_block_index < D_BLOCK) {
            if (inode->blocks[curr_block_index] == 0) {
                inode->blocks[curr_block_index] = get_free_block_bit();

                if (inode->blocks[curr_block_index] == -1) {
                    return wfs_error;
                }
            }
            block_ptr = disk + inode->blocks[curr_block_index];
        } else {

            char *indirect_block = disk + inode->blocks[D_BLOCK];
            off_t *indirect_block_ptr = (off_t *)indirect_block;
            int indirect_index = curr_block_index - D_BLOCK;

            if (indirect_index < (BLOCK_SIZE / sizeof(off_t))) {
                if (indirect_block_ptr[indirect_index] == 0) {
                    indirect_block_ptr[indirect_index] = get_free_block_bit();
                    if (indirect_block_ptr[indirect_index] == -1) {
                        return wfs_error;
                    }
                }
                block_ptr = disk + indirect_block_ptr[indirect_index];
            } else {
                break;
            }
        }

        if (block_ptr != NULL) {
            char temp_buf[BLOCK_SIZE];
            memcpy(temp_buf, block_ptr, BLOCK_SIZE);

            memcpy(temp_buf + start_pt, buf + process, bytes_to_copy); 
            memcpy(block_ptr, temp_buf, BLOCK_SIZE);

            process += bytes_to_copy;
            remaining -= bytes_to_copy;
        }
        curr_block_index++;
    }

    return process;
}

static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi){
    struct wfs_inode *inode = get_child_inode(path);
    if (inode == NULL) {
        return wfs_error;
    }

    if ((inode->mode & __S_IFMT) != __S_IFDIR) { 
        wfs_error = -ENOENT;
        return wfs_error;
    }

    for (int i = 0; i < inode->size / BLOCK_SIZE; i++) {
        struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + inode->blocks[i]);
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
            if (dentry[j].num != 0) {
                if (filler(buf, dentry[j].name, NULL, 0) != 0) {
                    return 0;
                }
            }
        }
    }
    return 0;
}

static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod   = wfs_mknod,
    .mkdir   = wfs_mkdir,
    .unlink  = wfs_unlink,
    .rmdir   = wfs_rmdir,
    .read    = wfs_read,
    .write   = wfs_write,
    .readdir = wfs_readdir,
};

int main(int argc, char *argv[]) {
    char *disk_path = argv[1];

    int fd = open(disk_path, O_RDWR);
    if (fd == -1) {
        perror("error on opening disk img");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("error on getting file size");
        return -1;
    }

    disk = (char*)mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    super_block = (struct wfs_sb *)disk;
    close(fd);
    
    for (int i = 1; i < argc - 1; i++) {
        argv[i] = argv[i + 1];
    }
    argc--;

    return fuse_main(argc, argv, &ops, NULL);
}

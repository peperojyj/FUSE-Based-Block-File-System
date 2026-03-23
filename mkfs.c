#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "wfs.h"

int roundup(int numToRound, int multiple) {
    int remainder = numToRound % multiple;
    if (remainder == 0) {
        return numToRound;
    }
    return numToRound + multiple - remainder;
}

int main(int argc, char *argv[]) {
    int opt;
    char *disk_image = NULL;
    int num_inodes = 0;
    int num_data_blocks = 0;

    while ((opt = getopt(argc, argv, "d:i:b:")) != -1) {
        switch (opt) {
            case 'd':
                disk_image = optarg;
                break;
            case 'i':
                num_inodes = atoi(optarg);
                num_inodes = roundup(num_inodes, 32);
                break;
            case 'b':
                num_data_blocks = atoi(optarg);
                num_data_blocks = roundup(num_data_blocks, 32);
                break;
            default:
                return -1;
        }
    }

    int start_sb = 0;
    int start_i_bitmap = start_sb + sizeof(struct wfs_sb);
    int start_d_bitmap = start_i_bitmap + num_inodes / 8;
    int start_i_block = start_d_bitmap + num_data_blocks / 8;
    int start_d_block = start_i_block + (num_inodes * BLOCK_SIZE);
    int total_size = start_d_block + (num_data_blocks * BLOCK_SIZE);

    int fd = open(disk_image, O_RDWR);
    
    struct stat buf;
    fstat(fd, &buf);
    size_t disk_size = buf.st_size;

    if (disk_size < total_size) {
        perror("error setting size of disk image");
        close(fd);
        exit(1);
    }

    char *map = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memset(map, 0, total_size);

    struct wfs_sb super_block;

    super_block.num_inodes = num_inodes;
    super_block.num_data_blocks = num_data_blocks;
    super_block.i_bitmap_ptr = start_i_bitmap;
    super_block.d_bitmap_ptr = start_d_bitmap;
    super_block.i_blocks_ptr = start_i_block;
    super_block.d_blocks_ptr = start_d_block;

    struct wfs_inode root_inode = {
        .num = 0,
        .mode = __S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR,
        .uid = getuid(),
        .gid = getgid(),
        .size = BLOCK_SIZE,
        .nlinks = 1,
        .atim = time(NULL),
        .mtim = time(NULL),
        .ctim = time(NULL),
        .blocks[0] = super_block.d_blocks_ptr
    };

    unsigned int *inode_bitmap = (unsigned int *)(map + start_i_bitmap);
    *inode_bitmap |= 1; 
    
    unsigned int *block_bitmap = (unsigned int *)(map + start_d_bitmap);
    *block_bitmap |= 1; 

    memcpy(map, &super_block, sizeof(super_block));
    memcpy(map + start_i_block, &root_inode, sizeof(root_inode));

    return 0;
}

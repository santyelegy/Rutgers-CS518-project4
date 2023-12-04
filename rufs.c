/*
 *  Copyright (C) 2023 CS416 Rutgers CS
 *	Tiny File System
 *	File:	rufs.c
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>

#include "block.h"
#include "rufs.h"

char diskfile_path[PATH_MAX];

// Declare your in-memory data structures here
struct superblock sb;
/* 
 * Get available inode number from bitmap
 */
int get_avail_ino() {
    // Step 1: Read inode bitmap from disk
    bitmap_t inode_bitmap = (bitmap_t)malloc(BLOCK_SIZE);
    if (!inode_bitmap) {
        perror("Failed to allocate memory for inode bitmap");
        return -1;
    }

    // Assuming the inode bitmap is located at sb.i_bitmap_blk
    if (bio_read(sb.i_bitmap_blk, inode_bitmap) < 0) {
        perror("Failed to read inode bitmap from disk");
        free(inode_bitmap);
        return -1;
    }

    // Step 2: Traverse inode bitmap to find an available slot
    for (int i = 0; i < MAX_INUM; i++) {
        if (get_bitmap(inode_bitmap, i) == 0) { // Found a free inode
            // Step 3: Update inode bitmap and write to disk 
            set_bitmap(inode_bitmap, i);
            if (bio_write(sb.i_bitmap_blk, inode_bitmap) < 0) {
                perror("Failed to write updated inode bitmap to disk");
                free(inode_bitmap);
                return -1;
            }
            free(inode_bitmap);
            return i; // Return the available inode number
        }
    }

    // No available inode found
    free(inode_bitmap);
    return -1;
}

/* 
 * Get available data block number from bitmap
 */
int get_avail_blkno() {

	// Step 1: Read data block bitmap from disk
	bitmap_t data_bitmap = (bitmap_t)malloc(BLOCK_SIZE);
	if (!data_bitmap) {
		perror("Failed to allocate memory for data block bitmap");
		return -1;
	}

	// Assuming the data block bitmap is located at sb.d_bitmap_blk
	if (bio_read(sb.d_bitmap_blk, data_bitmap) < 0) {
		perror("Failed to read data block bitmap from disk");
		free(data_bitmap);
		return -1;
	}
	
	// Step 2: Traverse data block bitmap to find an available slot
	for (int i = 0; i < MAX_DNUM; i++) {
		if (get_bitmap(data_bitmap, i) == 0) { // Found a free data block
			// Step 3: Update data block bitmap and write to disk 
			set_bitmap(data_bitmap, i);
			if (bio_write(sb.d_bitmap_blk, data_bitmap) < 0) {
				perror("Failed to write updated data block bitmap to disk");
				free(data_bitmap);
				return -1;
			}
			free(data_bitmap);
			return i; // Return the available data block number
		}
	}
	// No available data block found
	free(data_bitmap);
	return -1;
}

/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {

	// Step 1: Get the inode's on-disk block number
	int block_num = sb.i_start_blk + ino/INODES_PER_BLOCK;

	// Step 2: Get offset of the inode in the inode on-disk block
	int offset = ino % INODES_PER_BLOCK;

	// Step 3: Read the block from disk and then copy into inode structure
	struct inode * inode_block = (struct inode *)malloc(BLOCK_SIZE);
	if (!inode_block) {
		perror("Failed to allocate memory for inode block");
		return -1;
	}
	if (bio_read(block_num, inode_block) < 0) {
		perror("Failed to read inode block from disk");
		free(inode_block);
		return -1;
	}
	memcpy(inode, &inode_block[offset], sizeof(struct inode));
	free(inode_block);

	return 0;
}

int writei(uint16_t ino, struct inode *inode) {

	// Step 1: Get the block number where this inode resides on disk
	int block_num = sb.i_start_blk + ino/INODES_PER_BLOCK;
	
	// Step 2: Get the offset in the block where this inode resides on disk
	int offset = ino % INODES_PER_BLOCK;

	// Step 3: Write inode to disk 
	struct inode * inode_block = (struct inode *)malloc(BLOCK_SIZE);
	if (!inode_block) {
		perror("Failed to allocate memory for inode block");
		return -1;
	}
	if (bio_read(block_num, inode_block) < 0) {
		perror("Failed to read inode block from disk");
		free(inode_block);
		return -1;
	}
	memcpy(&inode_block[offset], inode, sizeof(struct inode));
	if (bio_write(block_num, inode_block) < 0) {
		perror("Failed to write inode block to disk");
		free(inode_block);
		return -1;
	}
	free(inode_block);
	return 0;
}


/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {

	// Step 1: Call readi() to get the inode using ino (inode number of current directory)
	struct inode * inode = (struct inode *)malloc(sizeof(struct inode));
	int result = readi(ino, inode);
	if (result < 0) {
		perror("Failed to read inode");
		free(inode);
		return -1;
	}

	// Step 2: Get data block of current directory from inode
	// don't support indirect pointer
	for(int i = 0; i < inode->size; i++){
		// The directory pointer actually stores the block number of the data block
		if(inode->direct_ptr[i] != 0){
			struct dirent * dirent_block = (struct dirent *)malloc(BLOCK_SIZE);
			if (!dirent_block) {
				perror("Failed to allocate memory for dirent block");
				free(inode);
				return -1;
			}
			if (bio_read(inode->direct_ptr[i], dirent_block) < 0) {
				perror("Failed to read dirent block from disk");
				free(inode);
				free(dirent_block);
				return -1;
			}
			// Step 3: Read directory's data block and check each directory entry.
			//If the name matches, then copy directory entry to dirent structure
			for(int j = 0; j < DIRENTS_PER_BLOCK; j++){
				if(dirent_block[j].valid == 1){
					if(strcmp(dirent_block[j].name,fname) == 0){
						memcpy(dirent, &dirent_block[j], sizeof(struct dirent));
						free(inode);
						free(dirent_block);
						return 0;
					}
				}
			}
			free(dirent_block);
		}
	}
	// not find
	return -1;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	for(int i=0;i<dir_inode.size;i++){
		if(dir_inode.direct_ptr[i] != 0){
			struct dirent * dirent_block = (struct dirent *)malloc(BLOCK_SIZE);
			if (!dirent_block) {
				perror("Failed to allocate memory for dirent block");
				return -1;
			}
			if (bio_read(dir_inode.direct_ptr[i], dirent_block) < 0) {
				perror("Failed to read dirent block from disk");
				free(dirent_block);
				return -1;
			}
			for(int j=0;j<DIRENTS_PER_BLOCK;j++){
				if(dirent_block[j].valid == 0){
					// Step 2: Check if fname (directory name) is already used in other entries
					if(strcmp(dirent_block[j].name,fname) == 0){
						perror("Directory name already used in other entries");
						free(dirent_block);
						return -1;
					}
				}
			}
			free(dirent_block);
		}
	}
	// Step 3: Add directory entry in dir_inode's data block and write to disk
	struct dirent * new_dirent = (struct dirent *)malloc(sizeof(struct dirent));
	memset(new_dirent, 0, sizeof(struct dirent));
	if (!new_dirent) {
		perror("Failed to allocate memory for new dirent");
		return -1;
	}
	new_dirent->ino = f_ino;
	new_dirent->valid = 1;
	memcpy(new_dirent->name, fname, name_len);
	// find a free entry (valid == 0)
	for(int i=0;i<dir_inode.size;i++){
		struct dirent * dirent_block = (struct dirent *)malloc(BLOCK_SIZE);
		if (!dirent_block) {
			perror("Failed to allocate memory for dirent block");
			return -1;
		}
		if (bio_read(dir_inode.direct_ptr[i], dirent_block) < 0) {
			perror("Failed to read dirent block from disk");
			free(dirent_block);
			return -1;
		}
		for(int j=0;j<DIRENTS_PER_BLOCK;j++){
			if(dirent_block[j].valid == 0){
				memcpy(&dirent_block[j], new_dirent, sizeof(struct dirent));
				if (bio_write(dir_inode.direct_ptr[i], dirent_block) < 0) {
					perror("Failed to write dirent block to disk");
					free(dirent_block);
					free(new_dirent);
					return -1;
				}
				free(dirent_block);
				free(new_dirent);
				return 0;
			}
		}
		
	}
	// cannot find a free entry	
	if(dir_inode.size == 16){
		perror("cannot find a free entry");
		free(new_dirent);
		return -1;
	}
	// Allocate a new data block for this directory if it does not exist
	int new_block_num = get_avail_blkno();
	if (new_block_num < 0) {
		perror("Failed to get an available block for directory");
		free(new_dirent);
		return -1;
	}
	// allocate a new data block
	struct dirent * new_block = (struct dirent *)malloc(BLOCK_SIZE);
	//zero out the new data block
	memset(new_block, 0, BLOCK_SIZE);
	// write the new directory entry to the new data block
	memcpy(new_block, new_dirent, sizeof(struct dirent));
	// write the new data block to disk
	if (bio_write(new_block_num, new_block) < 0) {
		perror("Failed to write new data block to disk");
		free(new_block);
		free(new_dirent);
		return -1;
	}
	// Update directory inode
	dir_inode.size += 1;
	dir_inode.direct_ptr[dir_inode.size-1] = new_block_num;
	// Write directory entry
	if (writei(dir_inode.ino, &dir_inode) < 0) {
		perror("Failed to write directory inode to disk");
		free(new_dirent);
		return -1;
	}
	free(new_dirent);
	return 0;
}

// skip this
int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
	
	// Step 2: Check if fname exist

	// Step 3: If exist, then remove it from dir_inode's data block and write to disk

	return 0;
}

/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way

	return 0;
}

/* 
 * Make file system
 */
int rufs_mkfs() {

	// Call dev_init() to initialize (Create) Diskfile
	dev_init(diskfile_path); // Initialize the disk file using dev_init(), using the diskfile_path as the file path

	// write superblock information
	struct superblock sb;
	sb.magic_num = MAGIC_NUM;
	sb.max_dnum = MAX_DNUM;
	sb.max_inum = MAX_INUM;

	sb.i_bitmap_blk = 1;
	sb.d_bitmap_blk = 2;
	
	int total_inode_size = (MAX_INUM*sizeof(struct inode) + BLOCK_SIZE -1); // Calculate the total size required for all inodes
	int number_of_inode_blocks = total_inode_size/BLOCK_SIZE; // Calculate the number of blocks required to store all inodes
	sb.i_start_blk = 3;
	sb.d_bitmap_blk = sb.i_start_blk + number_of_inode_blocks;
	
	// initialize inode bitmap (to zero using calloc)
	bitmap_t inode_bitmap = (bitmap_t)calloc(1,BLOCK_SIZE);
	if (!inode_bitmap) {
        perror("Failed to allocate inode bitmap");
        return -1;
    }
	bio_write(sb.i_bitmap_blk,inode_bitmap);  // Write the initialized inode bitmap to the disk

	// initialize data block bitmap
	bitmap_t data_bitmap = (bitmap_t)calloc(1, BLOCK_SIZE);
    if (!data_bitmap) {
        perror("Failed to allocate data block bitmap");
        free(inode_bitmap);
        return -1;
    }
    bio_write(sb.d_bitmap_blk, data_bitmap); // Write the initialized data block bitmap to the disk

	// update bitmap information for root directory
	set_bitmap(inode_bitmap,0); // set the 0th bit to 1
	bio_write(sb.i_bitmap_blk, inode_bitmap); // rewrite

	// update inode for root directory
	struct inode root_inode;
    root_inode.ino = 0;
    root_inode.valid = 1;
    root_inode.size = 0;  // Initially, size is 0
    root_inode.type = S_IFDIR;  // Directory type
    root_inode.link = 2;  // Standard for directories

	// The first inode starts right after the inode bitmap
    bio_write(sb.i_start_blk, &root_inode);

    free(inode_bitmap);
    free(data_bitmap);

	return 0;
}


/* 
 * FUSE file operations
 */
static void *rufs_init(struct fuse_conn_info *conn) {
	struct stat stbuf;
	// Step 1a: If disk file is not found, call mkfs
	int result = stat(diskfile_path,&stbuf);
	if(result == 1){
		// does not exist
		rufs_mkfs();
	}else{
		dev_open(diskfile_path);
	}

	// Step 1b: If disk file is found, just initialize in-memory data structures
	// and read superblock from disk

	// possibly put the superblock, inode, and dirent in heap here (malloc)

	struct superblock sb;
	bio_read(0,&sb); // read super block (at location 0)

	return NULL;
}

static void rufs_destroy(void *userdata) {

	// Step 1: De-allocate in-memory data structures

	// Step 2: Close diskfile

}

static int rufs_getattr(const char *path, struct stat *stbuf) {

	// Step 1: call get_node_by_path() to get inode from path

	// Step 2: fill attribute of file into stbuf from inode

		stbuf->st_mode   = S_IFDIR | 0755;
		stbuf->st_nlink  = 2;
		time(&stbuf->st_mtime);

	return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

    return 0;
}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: Read directory entries from its data blocks, and copy them to filler

	return 0;
}


static int rufs_mkdir(const char *path, mode_t mode) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target directory to parent directory

	// Step 5: Update inode for target directory

	// Step 6: Call writei() to write inode to disk
	

	return 0;
}

//skip this
static int rufs_rmdir(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of target directory

	// Step 3: Clear data block bitmap of target directory

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory

	return 0;
}

static int rufs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target file to parent directory

	// Step 5: Update inode for target file

	// Step 6: Call writei() to write inode to disk

	return 0;
}

static int rufs_open(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

	return 0;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: copy the correct amount of data from offset to buffer

	// Note: this function should return the amount of bytes you copied to buffer
	return 0;
}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: Write the correct amount of data from offset to disk

	// Step 4: Update the inode info and write it to disk

	// Note: this function should return the amount of bytes you write to disk
	return size;
}

//skip this
static int rufs_unlink(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of target file

	// Step 3: Clear data block bitmap of target file

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory

	return 0;
}

static int rufs_truncate(const char *path, off_t size) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_release(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_flush(const char * path, struct fuse_file_info * fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_utimens(const char *path, const struct timespec tv[2]) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}


static struct fuse_operations rufs_ope = {
	.init		= rufs_init,
	.destroy	= rufs_destroy,

	.getattr	= rufs_getattr,
	.readdir	= rufs_readdir,
	.opendir	= rufs_opendir,
	.releasedir	= rufs_releasedir,
	.mkdir		= rufs_mkdir,
	.rmdir		= rufs_rmdir,

	.create		= rufs_create,
	.open		= rufs_open,
	.read 		= rufs_read,
	.write		= rufs_write,
	.unlink		= rufs_unlink,

	.truncate   = rufs_truncate,
	.flush      = rufs_flush,
	.utimens    = rufs_utimens,
	.release	= rufs_release
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

	return fuse_stat;
}


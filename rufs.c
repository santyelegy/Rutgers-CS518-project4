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
#define ROOT_INO 0
/*
 * Get available inode number from bitmap
 */
int get_avail_ino()
{
	// Step 1: Read inode bitmap from disk
	bitmap_t inode_bitmap = (bitmap_t)malloc(BLOCK_SIZE);
	if (!inode_bitmap)
	{
		perror("Failed to allocate memory for inode bitmap");
		return -1;
	}

	// Assuming the inode bitmap is located at sb.i_bitmap_blk
	if (bio_read(sb.i_bitmap_blk, inode_bitmap) < 0)
	{
		perror("Failed to read inode bitmap from disk");
		free(inode_bitmap);
		return -1;
	}

	// Step 2: Traverse inode bitmap to find an available slot
	for (int i = 0; i < MAX_INUM; i++)
	{
		if (get_bitmap(inode_bitmap, i) == 0)
		{ // Found a free inode
			// Step 3: Update inode bitmap and write to disk
			set_bitmap(inode_bitmap, i);
			if (bio_write(sb.i_bitmap_blk, inode_bitmap) < 0)
			{
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
int get_avail_blkno()
{

	// Step 1: Read data block bitmap from disk
	bitmap_t data_bitmap = (bitmap_t)malloc(BLOCK_SIZE);
	if (!data_bitmap)
	{
		perror("Failed to allocate memory for data block bitmap");
		return -1;
	}

	// Assuming the data block bitmap is located at sb.d_bitmap_blk
	if (bio_read(sb.d_bitmap_blk, data_bitmap) < 0)
	{
		perror("Failed to read data block bitmap from disk");
		free(data_bitmap);
		return -1;
	}

	// Step 2: Traverse data block bitmap to find an available slot
	for (int i = 0; i < MAX_DNUM; i++)
	{
		if (get_bitmap(data_bitmap, i) == 0)
		{ // Found a free data block
			// Step 3: Update data block bitmap and write to disk
			set_bitmap(data_bitmap, i);
			if (bio_write(sb.d_bitmap_blk, data_bitmap) < 0)
			{
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
int readi(uint16_t ino, struct inode *inode)
{
	// Step 1: Get the inode's on-disk block number
	int block_num = sb.i_start_blk + ino / INODES_PER_BLOCK;

	// Step 2: Get offset of the inode in the inode on-disk block
	int offset = ino % INODES_PER_BLOCK;

	// Step 3: Read the block from disk and then copy into inode structure
	struct inode *inode_block = (struct inode *)malloc(BLOCK_SIZE);
	if (!inode_block)
	{
		perror("Failed to allocate memory for inode block");
		return -1;
	}
	if (bio_read(block_num, inode_block) < 0)
	{
		perror("Failed to read inode block from disk");
		free(inode_block);
		return -1;
	}
	memcpy(inode, &inode_block[offset], sizeof(struct inode));
	free(inode_block);

	return 0;
}

int writei(uint16_t ino, struct inode *inode)
{
	// Step 1: Get the block number where this inode resides on disk
	int block_num = sb.i_start_blk + ino / INODES_PER_BLOCK;

	// Step 2: Get the offset in the block where this inode resides on disk
	int offset = ino % INODES_PER_BLOCK;

	// Step 3: Write inode to disk
	struct inode *inode_block = (struct inode *)malloc(BLOCK_SIZE);
	if (!inode_block)
	{
		perror("Failed to allocate memory for inode block");
		return -1;
	}
	if (bio_read(block_num, inode_block) < 0)
	{
		perror("Failed to read inode block from disk");
		free(inode_block);
		return -1;
	}
	memcpy(&inode_block[offset], inode, sizeof(struct inode));
	if (bio_write(block_num, inode_block) < 0)
	{
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
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent)
{
	// get first name_len characters of fname
	char *name = (char *)malloc(name_len + 1);
	strncpy(name, fname, name_len);
	name[name_len] = '\0';
	
	// printf("calling dir_find with parameters: ino: %d, fname: %s, name_len: %d, name: %s\n", ino, fname, name_len, name);

	// Step 1: Call readi() to get the inode using ino (inode number of current directory)
	struct inode *inode = (struct inode *)malloc(sizeof(struct inode));
	int result = readi(ino, inode);
	if (result < 0)
	{
		perror("Failed to read inode");
		free(inode);
		return -1;
	}
	
	// Step 2: Get data block of current directory from inode
	// don't support indirect pointer
	for (int i = 0; i < inode->size; i++)
	{
		// The directory pointer actually stores the block number of the data block
		if (inode->direct_ptr[i] != 0)
		{
			struct dirent *dirent_block = (struct dirent *)malloc(BLOCK_SIZE);
			if (!dirent_block)
			{
				perror("Failed to allocate memory for dirent block");
				free(inode);
				free(name);
				return -1;
			}
			if (bio_read(inode->direct_ptr[i], dirent_block) < 0)
			{
				perror("Failed to read dirent block from disk");
				free(inode);
				free(dirent_block);
				free(name);
				return -1;
			}
			// Step 3: Read directory's data block and check each directory entry.
			// If the name matches, then copy directory entry to dirent structure

			for (int j = 0; j < DIRENTS_PER_BLOCK; j++)
			{
				if (dirent_block[j].valid == 1)
				{
					if (strcmp(dirent_block[j].name, name) == 0)
					{
						// printf("find directory entry with name: %s, ino: %d\n", dirent_block[j].name, dirent_block[j].ino);
						memcpy(dirent, &dirent_block[j], sizeof(struct dirent));
						free(inode);
						free(dirent_block);
						free(name);
						return 0;
					}
				}
			}
			free(dirent_block);
		}
	}
	// not find
	free(inode);
	free(name);
	return -1;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len)
{
	// // printf("calling dir_add with parameters: dir_inode: %d, f_ino: %d, fname: %s, name_len: %d\n", dir_inode.ino, f_ino, fname, name_len);

	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	for (int i = 0; i < dir_inode.size; i++)
	{
		if (dir_inode.direct_ptr[i] != 0)
		{
			struct dirent *dirent_block = (struct dirent *)malloc(BLOCK_SIZE);
			if (!dirent_block)
			{
				perror("Failed to allocate memory for dirent block");
				return -1;
			}
			if (bio_read(dir_inode.direct_ptr[i], dirent_block) < 0)
			{
				perror("Failed to read dirent block from disk");
				free(dirent_block);
				return -1;
			}
			for (int j = 0; j < DIRENTS_PER_BLOCK; j++)
			{
				if (dirent_block[j].valid == 0)
				{
					// Step 2: Check if fname (directory name) is already used in other entries
					if (strcmp(dirent_block[j].name, fname) == 0)
					{
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
	struct dirent *new_dirent = (struct dirent *)malloc(sizeof(struct dirent));
	memset(new_dirent, 0, sizeof(struct dirent));
	if (!new_dirent)
	{
		perror("Failed to allocate memory for new dirent");
		return -1;
	}
	new_dirent->ino = f_ino;
	new_dirent->valid = 1;
	memcpy(new_dirent->name, fname, name_len);
	// find a free entry (valid == 0)
	for (int i = 0; i < dir_inode.size; i++)
	{
		struct dirent *dirent_block = (struct dirent *)malloc(BLOCK_SIZE);
		if (!dirent_block)
		{
			perror("Failed to allocate memory for dirent block");
			return -1;
		}
		if (bio_read(dir_inode.direct_ptr[i], dirent_block) < 0)
		{
			perror("Failed to read dirent block from disk");
			free(dirent_block);
			return -1;
		}
		for (int j = 0; j < DIRENTS_PER_BLOCK; j++)
		{
			if (dirent_block[j].valid == 0)
			{
				memcpy(&dirent_block[j], new_dirent, sizeof(struct dirent));
				if (bio_write(dir_inode.direct_ptr[i], dirent_block) < 0)
				{
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
	if (dir_inode.size == 16)
	{
		perror("cannot find a free entry");
		free(new_dirent);
		return -1;
	}
	// Allocate a new data block for this directory if it does not exist
	int new_block_num = get_avail_blkno();
	if (new_block_num < 0)
	{
		perror("Failed to get an available block for directory");
		free(new_dirent);
		return -1;
	}
	// allocate a new data block
	struct dirent *new_block = (struct dirent *)malloc(BLOCK_SIZE);
	// zero out the new data block
	memset(new_block, 0, BLOCK_SIZE);
	// write the new directory entry to the new data block
	memcpy(new_block, new_dirent, sizeof(struct dirent));
	// write the new data block to disk
	if (bio_write(new_block_num, new_block) < 0)
	{
		perror("Failed to write new data block to disk");
		free(new_block);
		free(new_dirent);
		return -1;
	}
	// // printf("DIR_ADD: adding new block, new block number: %d\n", new_block_num);
	// Update directory inode
	dir_inode.size += 1;
	dir_inode.direct_ptr[dir_inode.size - 1] = new_block_num;
	// Update bitmap
	bitmap_t data_bitmap = (bitmap_t)malloc(BLOCK_SIZE);
	if (!data_bitmap)
	{
		perror("Failed to allocate memory for data block bitmap");
		free(new_block);
		free(new_dirent);
		return -1;
	}
	if (bio_read(sb.d_bitmap_blk, data_bitmap) < 0)
	{
		perror("Failed to read data block bitmap from disk");
		free(new_block);
		free(new_dirent);
		free(data_bitmap);
		return -1;
	}
	set_bitmap(data_bitmap, new_block_num);
	// Write directory inode to disk
	if (writei(dir_inode.ino, &dir_inode) < 0)
	{
		perror("Failed to write directory inode to disk");
		free(new_block);
		free(new_dirent);
		free(data_bitmap);
		return -1;
	}
	free(new_dirent);
	free(new_block);
	free(data_bitmap);
	return 0;
}

// skip this
int dir_remove(struct inode dir_inode, const char *fname, size_t name_len)
{

	// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode

	// Step 2: Check if fname exist

	// Step 3: If exist, then remove it from dir_inode's data block and write to disk

	return 0;
}

/*
 * namei operation
 * This is the actual namei function which follows a pathname until a terminal point is found.
 * To implement this function use the path, the inode number of the root of this path as input,
 * then call dir_find() to lookup each component in the path,
 * and finally read the inode of the terminal point to "struct inode *inode".
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode)
{
	// printf("calling get_node_by_path with parameters: path: %s, ino: %d\n", path, ino);

	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way
	// if input path is '/', return ino
	if (strcmp(path, "/") == 0 || strcmp(path, "") == 0)
	{

		readi(ino, inode);
		// printf("find directory inode with id: %d, type: %d expected type: %d, size: %d\n", ino, inode->type, S_IFDIR, inode->size);
		// // printf("Successfully find directory inode with id: %d\n", inode->ino);
		return 0;
	}
	// else, skip the first '/'
	if (path[0] == '/')
	{
		path++;
	}
	// printf("path: %s\n", path);
	size_t name_len = 0;
	for (int i = 0; i < strlen(path); i++)
	{
		if (path[i] == '/')
		{
			break;
		}
		else
		{
			name_len++;
		}
	}

	// check end condition, if inode is a file, return
	readi(ino, inode);
	// if is file
	if (inode-> valid && inode->type == S_IFREG)
	{
		// printf("find file inode: %d, name_len: %d, name: %s type: %d expected type: %d\n", ino, name_len, path, inode->type, S_IFREG);
		return 0;
	}
	struct dirent *dirent = (struct dirent *)malloc(sizeof(struct dirent));
	int success = dir_find(ino, path, name_len, dirent);
	// printf("dir find result %d\n", success);

	if (success < 0)
	{
		// not found
		return -ENOENT;
	}

	// if is directory
	if(name_len == strlen(path)){
		// end condition
		readi(dirent->ino, inode);
		// printf("find directory inode with id: %d, type: %d expected type: %d, size: %d\n", dirent->ino, inode->type, S_IFDIR, inode->size);
		return 0;
	}
	// recursive implementation
	// + 1 to skip the '/'
	success = get_node_by_path(path + name_len + 1, dirent->ino, inode);
	if (success < 0)
	{
		// not found
		return -ENOENT;
	}
	return 0;
}

/*
 * Make file system
 */
int rufs_mkfs()
{

	// Call dev_init() to initialize (Create) Diskfile
	dev_init(diskfile_path); // Initialize the disk file using dev_init(), using the diskfile_path as the file path

	// write superblock information
	sb.magic_num = MAGIC_NUM;
	sb.max_dnum = MAX_DNUM;
	sb.max_inum = MAX_INUM;

	sb.i_bitmap_blk = 1;
	sb.d_bitmap_blk = 2;

	int total_inode_size = (MAX_INUM * sizeof(struct inode) + BLOCK_SIZE - 1); // Calculate the total size required for all inodes
	int number_of_inode_blocks = total_inode_size / BLOCK_SIZE;				   // Calculate the number of blocks required to store all inodes
	sb.i_start_blk = sb.d_bitmap_blk + 1;
	sb.d_start_blk = sb.i_start_blk + number_of_inode_blocks;

	// write super block to disk
	char temp_buffer[BLOCK_SIZE];
	memset(temp_buffer, 0, BLOCK_SIZE);
	memcpy(temp_buffer, &sb, sizeof(sb));
	bio_write(0, temp_buffer);

	// initialize inode bitmap (to zero using calloc)
	bitmap_t inode_bitmap = (bitmap_t)calloc(1, BLOCK_SIZE);
	if (!inode_bitmap)
	{
		perror("Failed to allocate inode bitmap");
		return -1;
	}
	bio_write(sb.i_bitmap_blk, inode_bitmap); // Write the initialized inode bitmap to the disk

	// initialize data block bitmap
	bitmap_t data_bitmap = (bitmap_t)calloc(1, BLOCK_SIZE);
	if (!data_bitmap)
	{
		perror("Failed to allocate data block bitmap");
		free(inode_bitmap);
		return -1;
	}
	for(int i=0;i< sb.d_start_blk; i++){
		set_bitmap(data_bitmap, i);
	}
	bio_write(sb.d_bitmap_blk, data_bitmap); // Write the initialized data block bitmap to the disk

	// update inode for root directory
	struct inode root_inode;
	root_inode.ino = 0;
	root_inode.valid = 1;
	root_inode.size = 0;	   // Initially, size is 0
	root_inode.type = S_IFDIR; // Directory type
	root_inode.link = 2;	   // Standard for directories

	// The first inode starts right after the inode bitmap
	writei(0, &root_inode);
	// update bitmap information for root directory
	set_bitmap(inode_bitmap, 0);			  // set the 0th bit to 1
	bio_write(sb.i_bitmap_blk, inode_bitmap); // rewrite

	free(inode_bitmap);
	free(data_bitmap);

	return 0;
}

/*
 * FUSE file operations
 */
static void *rufs_init(struct fuse_conn_info *conn)
{
	// Step 1a: If disk file is not found, call mkfs
	// seems that this does not work
	rufs_mkfs();
	if (dev_open(diskfile_path) == -1)
	{
	}
	else
	{
		// read superblock from memory
		char temp_buffer[BLOCK_SIZE];
		bio_read(0, temp_buffer);
		memcpy(&sb, temp_buffer, sizeof(sb));
	}

	// Step 1b: If disk file is found, just initialize in-memory data structures
	// and read superblock from disk

	// possibly put the superblock, inode, and dirent in heap here (malloc)

	return NULL;
}

static void rufs_destroy(void *userdata)
{

	// Step 1: De-allocate in-memory data structures

	// Step 2: Close diskfile
	dev_close();
}

static int rufs_getattr(const char *path, struct stat *stbuf)
{

	// Step 1: call get_node_by_path() to get inode from path
	// printf("calling rufs_getattr with parameters: path: %s\n", path);
	struct inode *inode = (struct inode *)malloc(sizeof(struct inode));
	int success = get_node_by_path(path, ROOT_INO, inode);
	if (success < 0)
	{
		// file not found
		return -ENOENT;
	}

	// Step 2: fill attribute of file into stbuf from inode
	stbuf->st_ino = inode->ino;
	stbuf->st_size = inode->size * BLOCK_SIZE;
	stbuf->st_uid = getuid();
	stbuf->st_gid = getgid();

	if (inode->type == S_IFREG)
	{
		stbuf->st_nlink = 1;
		stbuf->st_mode = S_IFREG | 0755;
	}
	else
	{
		stbuf->st_nlink = 2;
		stbuf->st_mode = S_IFDIR | 0755;
	}
	time(&stbuf->st_mtime);
	// // printf("Successfully get file attribute with id: %d\n", inode->ino);
	return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi)
{

	// Step 1: Call get_node_by_path() to get inode from path
	struct inode *inode = (struct inode *)malloc(sizeof(struct inode));
	int success = get_node_by_path(path, ROOT_INO, inode);
	// Step 2: If not find, return -1
	if (success < 0)
	{
		// file not found
		return -1;
	}

	return 0;
}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{

	// Step 1: Call get_node_by_path() to get inode from path
	struct inode *inode = (struct inode *)malloc(sizeof(struct inode));
	int success = get_node_by_path(path, ROOT_INO, inode);
	if (success < 0)
	{
		// file not found
		return -ENOENT;
	}

	// Step 2: Read directory entries from its data blocks, and copy them to filler
	for (int i = 0; i < inode->size; i++)
	{
		if (inode->direct_ptr[i] != 0)
		{
			struct dirent *dirent_block = (struct dirent *)malloc(BLOCK_SIZE);
			if (!dirent_block)
			{
				perror("Failed to allocate memory for dirent block");
				return -1;
			}
			if (bio_read(inode->direct_ptr[i], dirent_block) < 0)
			{
				perror("Failed to read dirent block from disk");
				free(dirent_block);
				return -1;
			}
			for (int j = 0; j < DIRENTS_PER_BLOCK; j++)
			{
				if (dirent_block[j].valid == 1)
				{
					filler(buffer, dirent_block[j].name, NULL, 0);
				}
			}
			free(dirent_block);
		}
	}

	return 0;
}

static int rufs_mkdir(const char *path, mode_t mode)
{

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	char *path_copy1 = strdup(path);
	char *path_copy2 = strdup(path);
	char *dir_path = dirname(path_copy1);
	char *file_name = basename(path_copy2);
	// printf("calling rufs_mkdir with parameters: dir_path: %s, file_name: %s\n", dir_path, file_name);

	// Step 2: Call get_node_by_path() to get inode of parent directory
	struct inode parent_inode;
	if (get_node_by_path(dir_path, ROOT_INO, &parent_inode) < 0)
	{
		// parent directory not found
		// printf("parent directory not found\n");
		free(path_copy1);
		free(path_copy2);
		return -ENOENT;
	}
	
	// Step 3: Call get_avail_ino() to get an available inode number
	int ino = get_avail_ino();

	// Step 4: Call dir_add() to add directory entry of target directory to parent directory
	// printf("adding directory entry with ino: %d to parent directory with id: %d\n",ino, parent_inode.ino);
	if (dir_add(parent_inode, ino, file_name, strlen(file_name)) == -1)
	{
		// failed to add directory entry
		free(path_copy1);
		free(path_copy2);
		return -1;
	}
	// Step 5: Update inode for target directory
	struct inode new_inode;
	new_inode.ino = ino;
	new_inode.valid = 1;
	new_inode.size = 0;		 // New directory, so size is 0
	new_inode.type = S_IFDIR; // Directory type
	new_inode.link = 2;		 // Initial link count
	// Initialize direct and indirect pointers to 0
	for (int i = 0; i < 16; i++){
		new_inode.direct_ptr[i] = 0;
	}
	// Step 6: Call writei() to write inode to disk
	writei(ino, &new_inode);

	return 0;
}

// skip this
static int rufs_rmdir(const char *path)
{

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of target directory

	// Step 3: Clear data block bitmap of target directory

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory

	return 0;
}

static int rufs_releasedir(const char *path, struct fuse_file_info *fi)
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	// printf("CREATE: In rufs_create\n");

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name
	char *path_copy1 = strdup(path);
	char *path_copy2 = strdup(path);
	char *dir_path = dirname(path_copy1);
	char *file_name = basename(path_copy2);

	// Step 2: Call get_node_by_path() to get inode of parent directory
	struct inode parent_inode;
	if (get_node_by_path(dir_path, ROOT_INO, &parent_inode) < 0)
	{
		// parent directory not found
		free(path_copy1);
		free(path_copy2);
		// // printf("parent directory not found\n");
		return -ENOENT;
	}
	// // printf("Successfully get parent inode with id: %d\n", parent_inode.ino);

	// Step 3: Call get_avail_ino() to get an available inode number
	int ino = get_avail_ino();
	// // printf("get_avail_ino: %d\n", ino);
	if (ino == -1)
	{
		// no availiable inode
		free(path_copy1);
		free(path_copy2);
		return -1;
	}

	// Step 4: Call dir_add() to add directory entry of target file to parent directory
	if (dir_add(parent_inode, ino, file_name, strlen(file_name)) == -1)
	{
		// failed to add directory entry
		free(path_copy1);
		free(path_copy2);
		return -1;
	}

	// Step 5: Update inode for target file
	struct inode new_inode;
	new_inode.ino = ino;
	new_inode.valid = 1;
	new_inode.size = 0;		  // New file, so size is 0
	new_inode.type = S_IFREG; // Regular file type
	new_inode.link = 1;		  // Initial link count
							  // Initialize direct and indirect pointers to 0
	for (int i = 0; i < 16; i++)
		new_inode.direct_ptr[i] = 0;
	for (int i = 0; i < 8; i++)
		new_inode.indirect_ptr[i] = 0;
	
	// printf("creating new inode with id: %d\n", ino);

	// Step 6: Call writei() to write inode to disk
	writei(ino, &new_inode);

	free(path_copy1);
	free(path_copy2);

	// // printf("Successfully create file with id: %d\n", ino);
	return 0;
}

static int rufs_open(const char *path, struct fuse_file_info *fi)
{

	// Step 1: Call get_node_by_path() to get inode from path
	struct inode file_inode;
	if (get_node_by_path(path, ROOT_INO, &file_inode) == -1)
	{
		// file not found
		return -1;
	}
	// Step 2: If not find, return -1
	// use fi pointer??

	return 0;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
	// // printf("calling rufs_read with parameters: path: %s, size: %d, offset: %d\n", path, size, offset);

	// Step 1: You could call get_node_by_path() to get inode from path
	struct inode file_inode;
	if (get_node_by_path(path, ROOT_INO, &file_inode) == -1)
	{
		// File is not found
		return -1;
	}
	// printf("get inode with id %d file size: %d\n", file_inode.ino, file_inode.size);
	if (offset >= file_inode.size * BLOCK_SIZE)
	{
		// No data is read
		return 0;
	}
	if (offset + size > file_inode.size * BLOCK_SIZE)
	{
		// Adjust size if offset + size is beyond the end of the file
		size = file_inode.size * BLOCK_SIZE - offset;
	}

	// Step 2: Based on size and offset, read its data blocks from disk
	char temp_block[BLOCK_SIZE];
	size_t bytes_read = 0;
	while (bytes_read < size)
	{
		int block_num = (offset + bytes_read) / BLOCK_SIZE;
		int block_offset = (offset + bytes_read) % BLOCK_SIZE;
		int space_in_block = BLOCK_SIZE - block_offset;
		int bytes_to_read = space_in_block < (size - bytes_read) ? space_in_block : (size - bytes_read);
		if (block_num >= 16)
		{
			perror("File size exceeds maximum file size");
			return -1;
		}
		// Step 2: Read the block from disk if partial read
		if (block_offset != 0 || bytes_to_read < BLOCK_SIZE)
		{
			bio_read(file_inode.direct_ptr[block_num], temp_block);
		}
		// Read block from disk into temp block
		// // printf("reading block %d, bytes_read %d\n", file_inode.direct_ptr[block_num], bytes_read);
		bio_read(file_inode.direct_ptr[block_num], temp_block);
		// Step 3: copy the correct amount of data from offset to buffer
		memcpy(buffer + bytes_read, temp_block + block_offset, bytes_to_read);
		bytes_read += bytes_to_read;
	}

	// Note: this function should return the amount of bytes you copied to buffer
	return bytes_read;
}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
	// Step 1: You could call get_node_by_path() to get inode from path
	struct inode file_inode;
	if (get_node_by_path(path, ROOT_INO, &file_inode) == -1)
	{
		// File not found
		return -1;
	}
	// load block bitmap
	bitmap_t data_bitmap = (bitmap_t)malloc(BLOCK_SIZE);
	if (!data_bitmap)
	{
		perror("Failed to allocate memory for data block bitmap");
		return -1;
	}
	if (bio_read(sb.d_bitmap_blk, data_bitmap) < 0)
	{
		perror("Failed to read data block bitmap from disk");
		free(data_bitmap);
		return -1;
	}
	// Step 2: Based on size and offset, read its data blocks from disk
	char temp_block[BLOCK_SIZE];
	size_t bytes_written = 0;
	while (bytes_written < size)
	{
		int block_num = (offset + bytes_written) / BLOCK_SIZE;
		int block_offset = (offset + bytes_written) % BLOCK_SIZE;
		int space_in_block = BLOCK_SIZE - block_offset;
		int bytes_to_write = space_in_block < (size - bytes_written) ? space_in_block : (size - bytes_written);
		if (block_num >= 16)
		{
			perror("File size exceeds maximum file size");
			free(data_bitmap);
			return -1;
		}

		// Read the block from disk if partial write
		if (block_offset != 0 || bytes_to_write < BLOCK_SIZE)
		{
			// printf("detect partial write, reading block %d\n", file_inode.direct_ptr[block_num]);
			bio_read(file_inode.direct_ptr[block_num], temp_block);
		}
		else
		{
			// allocate a new data block
			int new_block_num = get_avail_blkno();
			if (new_block_num < 0)
			{
				perror("Failed to get an available block for directory");
				free(data_bitmap);
				return -1;
			}
			file_inode.direct_ptr[block_num] = new_block_num;
			// update inode size
			// the reason using block_num+1 is that the offset might be greater than original
			// file size, so we need to update the size to the offset
			file_inode.size = block_num + 1;
		}

		// Write the data from buffer to the temporary block
		memcpy(temp_block + block_offset, buffer + bytes_written, bytes_to_write);

		// printf("writing block %d\n", file_inode.direct_ptr[block_num]);
		// Write the modified block back to disk
		bio_write(file_inode.direct_ptr[block_num], temp_block);

		// Update bytes_written
		bytes_written += bytes_to_write;
		free(data_bitmap);
	}

	// Step 4: Update the inode info and write it to disk
	writei(file_inode.ino, &file_inode);
	// Note: this function should return the amount of bytes you write to disk
	return bytes_written;
}

// skip this
static int rufs_unlink(const char *path)
{

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of target file

	// Step 3: Clear data block bitmap of target file

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory

	return 0;
}

static int rufs_truncate(const char *path, off_t size)
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_release(const char *path, struct fuse_file_info *fi)
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_flush(const char *path, struct fuse_file_info *fi)
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_utimens(const char *path, const struct timespec tv[2])
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static struct fuse_operations rufs_ope = {
	.init = rufs_init,
	.destroy = rufs_destroy,

	.getattr = rufs_getattr,
	.readdir = rufs_readdir,
	.opendir = rufs_opendir,
	.releasedir = rufs_releasedir,
	.mkdir = rufs_mkdir,
	.rmdir = rufs_rmdir,

	.create = rufs_create,
	.open = rufs_open,
	.read = rufs_read,
	.write = rufs_write,
	.unlink = rufs_unlink,

	.truncate = rufs_truncate,
	.flush = rufs_flush,
	.utimens = rufs_utimens,
	.release = rufs_release};

int main(int argc, char *argv[])
{
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

	return fuse_stat;
}

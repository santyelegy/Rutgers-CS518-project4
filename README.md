# RUFS: use FUSE to build file system in user space

score 100/100

didn't implement extra credit

TODO:
- [ ] rufs_destroy
- [ ] rufs_getattr
- [ ] rufs_opendir
- [ ] rufs_readdir
- [ ] rufs_mkdir
- [ ] rufs_create
- [ ] rufs_open
- [ ] rufs_read
- [ ] rufs_write

rufs_getattr:

As mentioned in the project pdf, you need to fill out  st_uid, st_gid, st_nlink, st_size, st_mtime, and st_mode. You need to update the time whenever inodes are updated, that is when creating directories, files, or writing to a file.

As the project documentation pdf suggests, you first need to use path, to get the inode (get_node_by_path). 

Then, you need to fill out the st_uid, st_gid, st_nlink, st_size, st_mtime,  st_mode fields of stbuf. 

As mentioned in the pdf, you can set st_uid and set_gid to the values returned by getuid() and getgid().

st_link represents the number of hard links to the file. For directories, a sane default is 2 (the first link is the directory itself, and the second link is  ".") Technically, the correct number of links should be 2 + the number of subdirectories.  

For files, a sane default is 1 (the file itself).

Note that we won't be testing hard links.

Similarly, st_mode  can also be set to use sane defaults, as shown in the template. 

In rufs_mkfs you call dev_init to create the disk file, and initialize (malloc) your in-memory data structures. You initialize the superblock in memory, and then write it to disk.

In rufs_init, you call dev_open to check if the drive is initialized. If it is, you read the superblock in. If not, you call rufs_mkfs, which does the initializations for you, which will have initialized the superblock in memory before you write to disk it anyway. 

At the end of rufs_init, your in-memory superblock should be initialized. 

As such, FUSE doesn't mandate an rufs_mkfs, everything can be done in rufs_init. It's just separating out functionality. 

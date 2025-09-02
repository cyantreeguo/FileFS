/*----------------------------------------------------------------------------/
/  FileFS - Implement a virtual file system within a single file R1.0         /
/-----------------------------------------------------------------------------/
/
/ Copyright (C) 2025, cyantree, all right reserved.
/
/ mail: cyantree.guo@gmail.com
/ QQ: 9234933
/
/----------------------------------------------------------------------------*/

#ifndef FileFS_H_
#define FileFS_H_

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FileFS FileFS;

typedef struct FFS_FILE FFS_FILE;

typedef struct FFS_DIR FFS_DIR;
// d_type
#define FFS_DT_FILE 0
#define FFS_DT_DIR  1
#define FFS_DT_ROOT 2
typedef struct FFS_dirent FFS_dirent;
typedef struct FFS_dirent {
	/* File type */
	int d_type;
	
	/* Length of name without \0 */
	size_t d_namlen;

	/* File name */
	char d_name[15];
} FFS_dirent;

// =================================
FileFS *FileFS_create();
void FileFS_destroy(FileFS *ffs);

unsigned char FileFS_mkfs(const char *filename);
unsigned char FileFS_mount(FileFS *ffs, const char *filename);
void FileFS_umount(FileFS *ffs);
unsigned char FileFS_ismount(FileFS *ffs);

// =================================
/*
mode:
"r" 	(可读，不可写，必须存在) 打开一个用于读取的文件。该文件必须存在。
"w" 	(不可读，可写，无须存在，清空) 创建一个用于写入的空文件。如果文件名称与已存在的文件相同，则会删除已有文件的内容，文件被视为一个新的空文件。
"a" 	(不可读，可写，无须存在，追加) 追加到一个文件。写操作向文件末尾追加数据。如果文件不存在，则创建文件。
"r+" 	(可读，可写，必须存在) 打开一个用于更新的文件，可读取也可写入。该文件必须存在。
"w+" 	(可读，可写，无须存在，清空) 创建一个用于读写的空文件。
"a+" 	(可读，可写，无须存在，追加) 打开一个用于读取和追加的文件。若文件不存在返回NULL，打开后当前位置指向文件尾部
都是二进制格式读写
*/
FFS_FILE *FileFS_fopen(FileFS *ffs, const char *filename, const char *mode);
size_t FileFS_fread(FileFS *ffs, void *ptr, size_t size, size_t nmemb, FFS_FILE *stream);
size_t FileFS_fwrite(FileFS *ffs, const void *ptr, size_t size, size_t nmemb, FFS_FILE *stream);
void FileFS_fclose(FileFS *ffs, FFS_FILE *stream);

// fpos_t = int64 = long long
#define FFS_SEEK_CUR 1
#define FFS_SEEK_END 2
#define FFS_SEEK_SET 0
unsigned char FileFS_fseek(FileFS *ffs, FFS_FILE *stream, long long offset, int whence);
unsigned long long FileFS_ftell(FileFS *ffs, FFS_FILE *stream);
void FileFS_rewind(FileFS *ffs, FFS_FILE *stream);

// =================================
unsigned char FileFS_file_exist(FileFS *ffs, const char *filename);
unsigned char FileFS_dir_exist(FileFS *ffs, const char *pathname);
// return: 0-ok,1-gen err,2-file not exist,3-dir not existed,4-name>limit(14byte),5-name format err
int FileFS_remove(FileFS *ffs, const char *filename);
// return: 0:ok,1-err,2-old name format err,3-new name format err,4-old name not exist,5-new name exist, 6-old new format not match
int FileFS_rename(FileFS *ffs, const char *old_name, const char *new_name);
// return: 0:ok,1-err,2-from name format err,3-to path format err,4-from name not exist,5-to file exist, 6-from to format not match
int FileFS_move(FileFS *ffs, const char *from_name, const char *to_path);
// return: 0:ok,1-err,2-from name format err,3-to path format err,4-from name not exist,5-to file exist
int FileFS_copy(FileFS *ffs, const char *from_filename, const char *to_filename);
// return: 0-err,1-ok
unsigned char FileFS_chdir(FileFS *ffs, const char *pathname);
char *FileFS_getcwd(FileFS *ffs);
unsigned char FileFS_sethome(FileFS *ffs, const char *pathname); // ~
char *FileFS_gethome(FileFS *ffs);

// 将当前目录存储为工作目录，此接口只是为了保留当前目录
unsigned char FileFS_setwork(FileFS *ffs);
// 将当前目录转回工作目录
unsigned char FileFS_chwork(FileFS *ffs);

// return: 0-ok,1-gen err,2-name>limit(14byte),3-path existed,4-exist same name file
int FileFS_mkdir(FileFS *ffs, const char *pathname);
// return: 0-ok,1-gen err,2-sub dir not empty,3-path not existed,4-name>limit(14byte)
int FileFS_rmdir(FileFS *ffs, const char *pathname);

// =============================================
FFS_DIR *FileFS_opendir(FileFS *ffs, const char *path, char **absolute_path);
FFS_dirent *FileFS_readdir(FileFS *ffs, FFS_DIR *dir);
void FileFS_closedir(FileFS *ffs, FFS_DIR *dir);

// =================================
unsigned char FileFS_begin(FileFS *ffs);
unsigned char FileFS_commit(FileFS *ffs);
void FileFS_rollback(FileFS *ffs);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif

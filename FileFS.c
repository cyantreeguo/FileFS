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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "FileFS.h"

// =====================================
// platform depend start
// =====================================
// tested: winxp,win10,debian,haiku,freebsd

#define ffs_tmpfile() tmpfile()
#define ffs_fopen(filename, mode) fopen(filename, mode)
#define ffs_fwrite(ptr, size, nmemb, stream) fwrite(ptr, size, nmemb, stream)
#define ffs_fread(ptr, size, nmemb, stream) fread(ptr, size, nmemb, stream)
#define ffs_fclose(stream) fclose(stream)
#define ffs_remove(filename) remove(filename)
#define ffs_rewind(stream) rewind(stream)

static void ffs_fsetpos(FILE *fp, unsigned long long pos)
{
	fpos_t p;
#if defined(linux) || defined(__linux) || defined(__linux__)
#if defined(__GLIBC__)
	p.__pos = pos;
#else
	// musl
	p.__lldata = pos;
#endif
#else
	p = pos;
#endif
	fsetpos(fp, &p);
}

#if defined(WIN32) || defined(_WIN32) || defined(__CYGWIN__)
	#include <io.h>
	#include <errno.h>
	//#include <windows.h>
static void ffs_fflush(FILE *fp)
{
	fflush(fp);
	_commit(_fileno(fp));
	// HANDLE hFile= (HANDLE)_get_osfhandle(_fileno(fp));
	// FlushFileBuffers(hFile);
}
#elif defined(__HAIKU__)
	#include <unistd.h>
static void ffs_fflush(FILE *fp)
{
	fflush(fp);
	fsync(fileno(fp));
}
#else
	#include <unistd.h>
static void ffs_fflush(FILE *fp)
{
	fflush(fp);
	fdatasync(fileno(fp));
}
#endif

// =====================================
// platform depend stop
// =====================================

// block尺寸
#define BLOCKSIZE 512

// 一个block中最多能容纳的文件/目录项数量
#define BLOCK_ITEM_MAXCOUNT 20

// block的开头长度：tmpindex + nextblockindex + prevblockindex
#define BLOCK_HEAD 12

// 文件/目录名最大长度
#define BLOCK_NAME_MAXSIZE 14

// .->start_blockindex在block中的位置, head 12 + state 1 + name 14
#define BLOCK_START_BLOCKINDEX 27

// .->stop_blockindex在block中的位置, head 12 + state 1 + name 14 + start 4
#define BLOCK_STOP_BLOCKINDEX 31

// .->listsize在block中的位置, head 12 + state 1 + name 14 + start 4 + stop + 4
#define BLOCK_OFFSET 35

static unsigned char magic_number[4] = {0x78, 0x11, 0x45, 0x14};

typedef struct FFS_FILE {
	/*
	0 - "r" 	(可读，不可写，必须存在) 
	1 - "w" 	(不可读，可写，无须存在，清空)
	2 - "a" 	(不可读，可写，无须存在，追加) 
	3 - "r+" 	(可读，可写，必须存在) 
	4 - "w+" 	(可读，可写，无须存在，清空) 
	5 - "a+" 	(可读，可写，无须存在，追加)
	*/
	unsigned char mode;
	
	unsigned int dir_blockindex;
	unsigned short dir_offset;
	
	unsigned int file_start_blockindex, file_stop_blockindex;
	unsigned short file_offset;
	
	unsigned int pos_blockindex;
	unsigned short pos_offset;
	unsigned long long pos;
} FFS_FILE;

typedef struct FFS_DIR {
	unsigned int blockindex;
	unsigned char block[BLOCKSIZE];
	int searchindex; // 0 - BLOCK_ITEM_MAXCOUNT-1
	unsigned int stop_blockindex;
	unsigned short offset;
	
	FFS_dirent dirp;
} FFS_DIR;

typedef struct TMP TMP;
typedef struct TMP {
	unsigned char state; // 0-normal, 1-auto commit, 2-manu commit
	
	// 执行事务时pwd是独立的
	// 进入事务时需要将ffs->pwd复制到tmp.pwd，commit后需要再将tmp.pwd复制到ffs->pwd
	char *pwd;
	int pwd_size;
	unsigned int pwd_blockindex;
	
	char *home_pwd;
	int home_pwd_size;
	unsigned int home_pwd_blockindex;
	
	char *work;
	int work_size;
	unsigned int work_blockindex;
	
	// 4b(blockindex) + 512b(block)
	FILE *fp_cp, *fp_add;
	
	unsigned int cp_size;
	unsigned int add_size; // = new_total_blocksize
	
	unsigned int total_blocksize, unused_blockhead; // 执行fp = ffs_tmpfile()时，同步从orgfile里的block[0]读取这2个值
	unsigned int new_total_blocksize, new_unused_blockhead; // 一开始和上面的值相同，会随着tmpfile的处理产生变化
} TMP;

typedef struct FileFS {
	char *fn;
	FILE *fp;
	
	char *fnj;
	FILE *fpj;
	
	TMP tmp;
	
	char *pwd;
	int pwd_size;
	char *pwd_tmp;
	int pwd_tmp_size;
	unsigned int pwd_blockindex; // 当前目录所在的block
	
	char *home_pwd;
	int home_pwd_size;
	unsigned int home_pwd_blockindex;
	
	char *work;
	int work_size;
	unsigned int work_blockindex;
} FileFS;

// ==========================================
typedef struct BlockArray BlockArray;
typedef struct BlockArray {
	unsigned char active;
	unsigned char block[BLOCKSIZE];
	unsigned int blockindex;
} BlockArray;
	
static unsigned char tmpstart(FileFS *ffs, unsigned char state);
static void tmpstop(FileFS *ffs);
static unsigned int genblockindex(FileFS *ffs);
static unsigned char readblock(FileFS *ffs, unsigned int blockindex, unsigned char *block);
static unsigned char writeblock(FileFS *ffs, unsigned int blockindex, unsigned char *block);
static unsigned char removeblock(FileFS *ffs, unsigned int blockindex);

static unsigned int findPathBlockindex(FileFS *ffs, unsigned int blockindex, char *pathname);
static void j2ffs(FileFS *ffs);

// ==========================================
static unsigned int B4toU32(unsigned char byte[4])
{
	return (unsigned int)(
		( byte[0]      & 0xFF)     |
		((byte[1]<<8)  & 0xFF00)   |
		((byte[2]<<16) & 0xFF0000) |
		((byte[3]<<24) & 0xFF000000)
		);
}

static void U32toB4(unsigned int v, unsigned char byte[4])
{
	byte[0] = (unsigned char) ((v & 0x000000FF)); 
    byte[1] = (unsigned char) ((v & 0x0000FF00)>>8);
	byte[2] = (unsigned char) ((v & 0x00FF0000)>>16);
	byte[3] = (unsigned char) ((v & 0xFF000000)>>24);
}

static unsigned short B2toU16(unsigned char byte[2])
{
	return (unsigned int)(
		( byte[0]      & 0xFF) |
		((byte[1]<<8)  & 0xFF00)
		);
}

static void U16toB2(unsigned short v, unsigned char byte[2])
{
	byte[0] = (unsigned char) ((v & 0x00FF)); 
    byte[1] = (unsigned char) ((v & 0xFF00)>>8);
}

// =================================
FileFS *FileFS_create()
{
	FileFS *ffs = (FileFS*)malloc(sizeof(FileFS));
	if ( ffs == NULL ) return NULL;
	
	memset(ffs, 0, sizeof(FileFS));
	
	return ffs;
}

void FileFS_destroy(FileFS *ffs)
{
	if ( ffs == NULL ) return;
	
	FileFS_umount(ffs);
	
	free(ffs);
}

unsigned char FileFS_mkfs(const char *filename)
{
	FILE *fp;
	
	fp = ffs_fopen(filename, "wb");
	if ( fp == NULL ) return 0;
	
	unsigned char block[BLOCKSIZE];
	unsigned int n;
	unsigned char b4[4];
	int k;
	unsigned short offset;
	unsigned char b2[2];
	
	// block[0]
	memset(block, 0, BLOCKSIZE);
	k = 0;
	// magic number	
	memcpy(block+k, magic_number, 4); k += 4;
	// block size;
	n = 2;
	U32toB4(n, b4);
	memcpy(block+k, b4, 4); k += 4;
	// unused block head,此时为0
	// other,皆为0
	
	if ( BLOCKSIZE != ffs_fwrite(block, 1, BLOCKSIZE, fp) ) {
		ffs_fclose(fp);
		return 0;
	}
	
	// block[1],根目录
	unsigned char state;
	char name[BLOCK_NAME_MAXSIZE+1];
	memset(block, 0, BLOCKSIZE);
	k = 0;
	// tmpindex
	k += 4;
	// next block, 0
	k += 4;
	// prev block, 0
	k += 4;
	
	// dir
	// .
	// state
	state = 0;
	memcpy(block+k, &state, 1); k += 1;
	// name
	memset(name, 0, BLOCK_NAME_MAXSIZE+1);
	name[0] = '.';
	memcpy(block+k, name, 1); k += BLOCK_NAME_MAXSIZE;
	// start_blockindex, 1
	n = 1;
	U32toB4(n, b4);
	memcpy(block+k, b4, 4); k += 4;
	// stop_blockindex, 1
	memcpy(block+k, b4, 4); k += 4;
	// offset
	offset = 4+4+4 + 25 + 25; // tmpindex+nextblockindex+prevblockindex + . + ..
	U16toB2(offset, b2);
	memcpy(block+k, b2, 2); k += 2;
	
	// ..
	// state
	state = 0;
	memcpy(block+k, &state, 1); k += 1;
	// name
	name[1] = '.'; // ..
	memcpy(block+k, name, 2); k += BLOCK_NAME_MAXSIZE;
	// start_blockindex, 0
	k += 4;
	// stop_blockindex, 0
	k += 4;
	// offset, 0
	k += 2;
	
	if ( BLOCKSIZE != ffs_fwrite(block, 1, BLOCKSIZE, fp) ) {
		ffs_fclose(fp);
		return 0;
	}
	
	ffs_fflush(fp);
	ffs_fclose(fp);
	
	char *fnj = (char*)malloc(strlen(filename)+1+2);
	if ( fnj == NULL ) return 1;
	sprintf(fnj, "%s-j", filename);
	ffs_remove(fnj);
	free(fnj);
	
	return 1;
}

unsigned char FileFS_mount(FileFS *ffs, const char *filename)
{
	if ( ffs == NULL ) return 0;
	
	FILE *fp, *fpj;
	
	fp = ffs_fopen(filename, "r+b");
	if ( fp == NULL ) return 0;
	
	unsigned char block[BLOCKSIZE];
	// ===== block[0]
	if ( BLOCKSIZE != ffs_fread(block, 1, BLOCKSIZE, fp) ) {
		ffs_fclose(fp);
		return 0;
	}
	
	int k = 0;
	// magic number;
	unsigned char mn[4];
	memcpy(mn, block+k, 4); k += 4;
	//printf("%X %X %X %X\n", mn[0], mn[1], mn[2], mn[3]);
	if ( memcmp(mn, magic_number, 4) != 0 ) {
		ffs_fclose(fp);
		return 0;
	}
	// block size;
	unsigned int bs;
	unsigned char b4[4];
	memcpy(b4, block+k, 4); k+=4;
	bs = B4toU32(b4);
	if ( bs < 2 ) {
		ffs_fclose(fp);
		return 0;
	}
	
	// ===== block[1]
	if ( BLOCKSIZE != ffs_fread(block, 1, BLOCKSIZE, fp) ) {
		ffs_fclose(fp);
		return 0;
	}
	
	unsigned char state;
	char name[BLOCK_NAME_MAXSIZE+1];
	memset(name, 0, BLOCK_NAME_MAXSIZE+1);
	// block[1],根目录
	k = 0;
	// tmpindex
	k += 4;
	// next block	
	k += 4;
	// prev block
	k += 4;
	
	// dir
	// .
	// state
	memcpy(&state, block+k, 1); k += 1;
	if ( state != 0 ) {
		ffs_fclose(fp);
		return 0;
	}
	// name
	memcpy(name, block+k, BLOCK_NAME_MAXSIZE); k += BLOCK_NAME_MAXSIZE;
	if ( strcmp(name, ".") != 0 ) {
		ffs_fclose(fp);
		return 0;
	}
	// start_blockindex, 1
	k += 4;
	// stop_blockindex,0
	k += 4;
	// offset, 0
	k += 2;
	
	// ..
	// state
	memcpy(&state, block+k, 1); k += 1;
	if ( state != 0 ) {
		ffs_fclose(fp);
		return 0;
	}
	// name
	memcpy(name, block+k, BLOCK_NAME_MAXSIZE); k += BLOCK_NAME_MAXSIZE;
	if ( strcmp(name, "..") != 0 ) {
		ffs_fclose(fp);
		return 0;
	}
	// start_blockindex, 0
	//k += 4;
	// stop_blockindex, 0
	//k += 4;
	// offset, 0
	//k += 2;
	
	if ( ffs->fp != NULL ) {
		ffs_fclose(ffs->fp);
		ffs->fp = NULL;
	}
	if ( ffs->fn != NULL ) {
		free(ffs->fn);
		ffs->fn = NULL;
	}
	if ( ffs->fnj != NULL ) {
		free(ffs->fnj);
		ffs->fnj = NULL;
	}
	
	ffs->fp = fp;
	
	int len = (int)strlen(filename);
	ffs->fn = (char*)malloc(len+1);
	if ( ffs->fn == NULL ) {
		ffs_fclose(fp);
		return 0;
	}
	strcpy(ffs->fn, filename);
	
	// ============= fpj ==============
	ffs->fnj = (char*)malloc(len+1+2);
	if ( ffs->fnj == NULL ) {
		ffs_fclose(fp);
		return 0;
	}
	sprintf(ffs->fnj, "%s-j", ffs->fn);

	fpj = ffs_fopen(ffs->fnj, "w+b");
	if ( fpj == NULL ) {
		ffs_fclose(fp);
		ffs->fp = NULL;
		return 0;
	}
	ffs->fpj = fpj;
	
	// =========================================
	if ( ffs->pwd != NULL ) free(ffs->pwd);
	ffs->pwd = (char*)malloc(2);
	if ( ffs->pwd == NULL ) {
		ffs_fclose(ffs->fp);
		return 0;
	}
	sprintf(ffs->pwd, "/");
	ffs->pwd_size = 2;
	ffs->pwd_blockindex = 1;
	
	if ( ffs->home_pwd != NULL ) free(ffs->home_pwd);
	ffs->home_pwd = (char*)malloc(2);
	if ( ffs->home_pwd == NULL ) {
		ffs_fclose(ffs->fp);
		return 0;
	}
	sprintf(ffs->home_pwd, "/");
	ffs->home_pwd_size = 2;
	ffs->home_pwd_blockindex = 1;
	
	if ( ffs->work != NULL ) free(ffs->work);
	ffs->work = NULL;
	ffs->work_size = 0;
	ffs->work_blockindex = 1;
	
	// move data of fn-j to fn;
	j2ffs(ffs);
	
	return 1;
}

void FileFS_umount(FileFS *ffs)
{
	if ( ffs == NULL ) return;
	
	if ( ffs->fp != NULL ) {
		ffs_fclose(ffs->fp);
		ffs->fp = NULL;
	}
	if ( ffs->fn != NULL ) {
		free(ffs->fn);
		ffs->fn = NULL;
	}
	if (ffs->fpj != NULL) {
		ffs_fclose(ffs->fpj);
		ffs->fpj = NULL;
	}
	if ( ffs->fnj != NULL ) {
		ffs_remove(ffs->fnj);
		free(ffs->fnj);
		ffs->fnj = NULL;
	}
	
	if ( ffs->pwd != NULL ) {
		free(ffs->pwd);
		ffs->pwd = NULL;
	}
	ffs->pwd_size = 0;
	ffs->pwd_blockindex = 0;
	
	if ( ffs->pwd_tmp != NULL ) {
		free(ffs->pwd_tmp);
		ffs->pwd_tmp = NULL;
	}
	ffs->pwd_tmp_size = 0;
	
	if ( ffs->home_pwd != NULL ) {
		free(ffs->home_pwd);
		ffs->home_pwd = NULL;
	}
	ffs->home_pwd_size = 0;
	ffs->home_pwd_blockindex = 0;
	
	if ( ffs->work != NULL ) {
		free(ffs->work);
		ffs->work = NULL;
	}
	ffs->work_size = 0;
	ffs->work_blockindex = 0;
	
	// tmp
	if ( ffs->tmp.fp_cp != NULL ) {
		ffs_fclose(ffs->tmp.fp_cp);
		ffs->tmp.fp_cp = NULL;
	}
	if ( ffs->tmp.fp_add != NULL ) {
		ffs_fclose(ffs->tmp.fp_add);
		ffs->tmp.fp_add = NULL;
	}
	ffs->tmp.cp_size = ffs->tmp.add_size = 0;
	
	if ( ffs->tmp.pwd != NULL ) {
		free(ffs->tmp.pwd);
		ffs->tmp.pwd = NULL;
	}
	ffs->tmp.pwd_size = 0;
	if ( ffs->tmp.home_pwd != NULL ) {
		free(ffs->tmp.home_pwd);
		ffs->tmp.home_pwd = NULL;
	}
	ffs->tmp.home_pwd_size = 0;
	if ( ffs->tmp.work != NULL ) {
		free(ffs->tmp.work);
		ffs->tmp.work = NULL;
	}
	ffs->tmp.work_size = 0;
}

unsigned char FileFS_ismount(FileFS *ffs)
{
	if ( ffs == NULL ) return 0;
	
	if ( ffs->fp == NULL ) return 0;
	return 1;
}

// =================================
static FFS_FILE *do_fopen_r(FileFS *ffs, char *lastname, unsigned char mode, unsigned int block_head_index)
{
	/*
	0 - "r" 	(可读，不可写，必须存在)
	3 - "r+" 	(可读，可写，必须存在)
		if not exist, return NULL;
		new FFS_FILE;
		pos_blockindex = start_blockindex;
		pos_offset = 0;
		pos = 0;
		mode = bmode;
		return fp;
	*/
	
	unsigned char block[BLOCKSIZE];
	unsigned char state, dir_file;
	unsigned char b4[4], b2[2];
	unsigned int stop_blockindex;
	unsigned short offset;
	int k;
	
	// block_head
	if ( ! readblock(ffs, block_head_index, block) ) return NULL;
	memcpy(b4, block+(12+1+14+4), 4);
	stop_blockindex = B4toU32(b4);
	memcpy(b2, block+(12+1+14+4+4), 2);
	offset = B2toU16(b2);
	
	// 搜索block，检查是否有名称相同的文件
	unsigned char *dir_block = NULL;
	unsigned int dir_blockindex = 0;
	unsigned short dir_offset = 0;
	
	int i;
	unsigned char flag = 0;
	unsigned int index = block_head_index;
	char s[BLOCK_NAME_MAXSIZE+1];
	memset(s, 0, BLOCK_NAME_MAXSIZE+1);
	while (1) {
		// blocksize=512,去掉前置的12byte，一共能放下20个子项目(目录或文件)
		k = BLOCK_HEAD;
		for (i=0; i<BLOCK_ITEM_MAXCOUNT; i++) {
			if ( index == stop_blockindex && k+1 >= offset ) { // 已搜索到最后
				return NULL; // file not exist
			}
				
			state = block[k]; k++;
			memcpy(s, block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
			if ( strcmp(s, lastname) != 0 ) {
				k += 10; // 4+4+2
				continue;
			}
			dir_file = state & 0x01;
			if ( dir_file == 0 ) {
				return NULL; // same path exist;
			}
			
			dir_block = block;
			dir_blockindex = index;
			dir_offset = k + 10; // 位置在尾部
			
			flag = 1;
			break;
		}
		if ( flag ) break;
		
		// get nextblockindex;
		memcpy(b4, block+4, 4);
		index = B4toU32(b4);
		if ( index == 0 ) return NULL; // 到这里说明block有问题
		if ( ! readblock(ffs, index, block) ) return NULL; // 到这里说明block有问题
	}
	
	if ( dir_block == NULL ) return NULL;

	FFS_FILE *ff;
	ff = (FFS_FILE*)malloc(sizeof(FFS_FILE));
	if ( ff == NULL ) return NULL;
	
	ff->mode = mode;
	
	ff->dir_blockindex = dir_blockindex;
	ff->dir_offset = dir_offset;
	
	memcpy(b4, dir_block+dir_offset-10, 4);
	ff->file_start_blockindex = B4toU32(b4); // 若为0，则说明尚未创建包含文件内容的block
	memcpy(b4, dir_block+dir_offset-6, 4);
	ff->file_stop_blockindex = B4toU32(b4);
	memcpy(b2, dir_block+dir_offset-2, 2);
	ff->file_offset = B2toU16(b2);
	
	ff->pos_blockindex = ff->file_start_blockindex;
	ff->pos_offset = BLOCK_HEAD;
	
	ff->pos = 0;
	
	return ff;
}
// 创建文件item
static unsigned char do_fopen_createfileitem(FileFS *ffs, char *lastname, 
	unsigned int org_start_blockindex, unsigned int org_stop_blockindex, unsigned short org_offset,
	unsigned char *dir_block, unsigned int *dir_blockindex, unsigned short *dir_offset)
{
	/*
	if ( offset == BLOCKSIZE ) {
		new_blockindex = genblock;
		block_stop->nextblockindex = new_blockindex;
		add lastname to block_new;
		start_blockindex = stop_blockindex = offset = 0;
		block_head->stop_blockindex = new_blockindex;
		block_head->offset = 12 + 25;
	} else {
		add lastname to offset;
		start_blockindex = stop_blockindex = offset = 0;
		offset += 25;
	}
	*/
	
	int i, k;
	unsigned short new_offset;
	unsigned char b4[4], b2[2];
	BlockArray ba[2];
	int ba_used = 0;
	unsigned char *block_start, *block_stop;
	unsigned int block_start_index, block_stop_index;
	for (i=0; i<2; i++) ba[i].active = 0;
	
	// block_start
	if ( ! readblock(ffs, org_start_blockindex, ba[0].block) ) return 0;
	ba[0].blockindex = org_start_blockindex;
	ba[0].active = 1;
	block_start = ba[0].block;
	block_start_index = ba[0].blockindex;
	ba_used = 1;
	
	// block_stop
	if ( org_stop_blockindex == org_start_blockindex ) {
		block_stop = block_start;
		block_stop_index = block_start_index;
	} else {
		if ( ! readblock(ffs, org_stop_blockindex, ba[1].block) ) return 0;
		ba[1].blockindex = org_stop_blockindex;
		ba[1].active = 1;
		block_stop = ba[1].block;
		block_stop_index = ba[1].blockindex;
		ba_used++;
	}
	
	if ( ffs->tmp.state == 0 ) tmpstart(ffs, 1);

	// dir_block未填满
	if ( org_offset < BLOCKSIZE ) {
		// == 向org_stop_block写入lastname目录项
		k = org_offset;
		
		// state
		block_stop[k] = 1; // file 
		k++; 
		
		memset(block_stop+k, 0, BLOCK_NAME_MAXSIZE);
		memcpy(block_stop+k, lastname, (int)strlen(lastname));
		k += BLOCK_NAME_MAXSIZE;
		
		k += 4; // start_blockindex
		k += 4; // stop_blockindex
		k += 2; // offset
		new_offset = k;
		U16toB2(new_offset, b2);
		memcpy(block_start + BLOCK_OFFSET, b2, 2);
		
		for (i=0; i<2; i++) {
			if ( ba[i].active ) {
				if ( ! writeblock(ffs, ba[i].blockindex, ba[i].block) ) {
					if ( ffs->tmp.state == 1 ) tmpstop(ffs);
					return 0;
				}
			}
		}
			
		if ( ffs->tmp.state == 1 ) {
			if ( ! FileFS_commit(ffs) ) {
				return 0;
			}
		}
		
		memcpy(dir_block, block_stop, BLOCKSIZE);
		*dir_blockindex = block_stop_index;
		*dir_offset = new_offset;
		
		return 1;
	}
	
	// 最后一个block已填满
	// ======================================
	unsigned int blockindex_2;
	unsigned char block_2[BLOCKSIZE];
	
	// 创建存储lastname的目录延伸块
	// gen block_2 for lastname;
	// block_2->prevblockindex = cur_blockindex;
	// write;
	blockindex_2 = genblockindex(ffs);
	if ( blockindex_2 == 0 ) {
		if ( ffs->tmp.state == 1 ) tmpstop(ffs);
		return 0;
	}		
	memset(block_2, 0, BLOCKSIZE);
	k = 8;
	// prevblockindex
	U32toB4(org_stop_blockindex, b4);
	memcpy(block_2+k, b4, 4);
	k += 4;
	
	// state
	block_2[k] = 1; // file
	k++; 
	
	memset(block_2+k, 0, BLOCK_NAME_MAXSIZE);
	memcpy(block_2+k, lastname, (int)strlen(lastname));
	k += BLOCK_NAME_MAXSIZE;
	
	k += 4; // start_blockindex
	k += 4; // stop_blockindex
	k += 2; // offset
	new_offset = k;
	U16toB2(new_offset, b2);
	memcpy(block_start + BLOCK_OFFSET, b2, 2);
	
	U32toB4(blockindex_2, b4);
	memcpy(block_start + BLOCK_STOP_BLOCKINDEX, b4, 4);
	
	memcpy(block_stop + 4, b4, 4);
	
	for (i=0; i<2; i++) {
		if ( ba[i].active ) {
			if ( ! writeblock(ffs, ba[i].blockindex, ba[i].block) ) {
				if ( ffs->tmp.state == 1 ) tmpstop(ffs);
				return 0;
			}
		}
	}
		
	if ( ffs->tmp.state == 1 ) {
		if ( ! FileFS_commit(ffs) ) {
			return 0;
		}
	}
	
	memcpy(dir_block, block_2, BLOCKSIZE);
	*dir_blockindex = blockindex_2;
	*dir_offset = new_offset;
		
	return 1;
}
// 删除文件内容
static unsigned char do_fopen_cleanfilecontent(FileFS *ffs, unsigned char *dir_block, unsigned int dir_blockindex, unsigned short dir_offset)
{
	/*
	readblock(file->start_blockindex);
	readblock(file->stop_blockindex);
	stop_block->nextblockindex = new_unused_blockindex;
	ffs->tmp.new_unused_blockindex = start_blockindex;
	file->start_blockindex = 0;
	file->stop_blockindex = 0;
	file->offset = 0;
	*/
	unsigned char b4[4];
	unsigned int file_start_blockindex, file_stop_blockindex;
	
	memcpy(b4, dir_block+dir_offset-10, 4);
	file_start_blockindex = B4toU32(b4);
	memcpy(b4, dir_block+dir_offset-6, 4);
	file_stop_blockindex = B4toU32(b4);
	if ( file_start_blockindex == 0 ) return 1; // 文件存在，但无内容
	
	if ( ffs->tmp.state == 0 ) tmpstart(ffs, 1);
	
	// file block_stop
	unsigned char file_block_stop[BLOCKSIZE];
	if ( ! readblock(ffs, file_stop_blockindex, file_block_stop) ) {
		if ( ffs->tmp.state == 1 ) tmpstop(ffs);
		return 0;
	}
	
	U32toB4(ffs->tmp.new_unused_blockhead, b4);
	memcpy(file_block_stop + 4, b4, 4);
	ffs->tmp.new_unused_blockhead = file_start_blockindex;
	
	// set dir->start stop offset -> 0
	memset(dir_block + dir_offset - 10, 0, 10);
	
	if ( ! writeblock(ffs, dir_blockindex, dir_block) ) {
		if ( ffs->tmp.state == 1 ) tmpstop(ffs);
		return 0;
	}
	if ( ! writeblock(ffs, file_stop_blockindex, file_block_stop) ) {
		if ( ffs->tmp.state == 1 ) tmpstop(ffs);
		return 0;
	}
	
	if ( ffs->tmp.state == 1 ) {
		if ( ! FileFS_commit(ffs) ) {
			return 0;
		}
	}
	
	return 1;
}
static FFS_FILE *do_fopen_w(FileFS *ffs, char *lastname, unsigned char mode, unsigned int block_head_index)
{
	/*
	1 - "w" 	(不可读，可写，无须存在，清空)
	4 - "w+" 	(可读，可写，无须存在，清空)
		if exist {
			readblock(file->start_blockindex);
			readblock(file->stop_blockindex);
			stop_block->nextblockindex = new_unused_blockindex;
			ffs->tmp.new_unused_blockindex = start_blockindex;
			file->start_blockindex = 0;
			file->stop_blockindex = 0;
			file->offset = 0;
		} else {
			// new file item
			if ( offset == BLOCKSIZE ) {
				new_blockindex = genblock;
				block_stop->nextblockindex = new_blockindex;
				add lastname to block_new;
				start_blockindex = stop_blockindex = offset = 0;
				block_head->stop_blockindex = new_blockindex;
				block_head->offset = 12 + 25;
			} else {
				add lastname to offset;
				start_blockindex = stop_blockindex = offset = 0;
				offset += 25;
			}
		}
		new fp;
		pos_blockindex = 0;
		pos_offsize = 0;
		fp->pos = 0;
		fp->mode = bmode;
	*/
	
	unsigned char block[BLOCKSIZE];
	unsigned char state, dir_file;
	unsigned char b4[4], b2[2];
	unsigned int stop_blockindex;
	unsigned short offset;
	int k;
	
	// block_head
	if ( ! readblock(ffs, block_head_index, block) ) return NULL;
	memcpy(b4, block+(12+1+14+4), 4);
	stop_blockindex = B4toU32(b4);
	memcpy(b2, block+(12+1+14+4+4), 2);
	offset = B2toU16(b2);
	
	// 搜索block，检查是否有名称相同的文件
	unsigned char *dir_block = NULL;
	unsigned int dir_blockindex = 0;
	unsigned short dir_offset = 0;
	
	int i;
	unsigned char flag = 0;
	unsigned int index = block_head_index;
	char s[BLOCK_NAME_MAXSIZE+1];
	memset(s, 0, BLOCK_NAME_MAXSIZE+1);
	while (1) {
		// blocksize=512,去掉前置的12byte，一共能放下20个子项目(目录或文件)
		k = BLOCK_HEAD;
		for (i=0; i<BLOCK_ITEM_MAXCOUNT; i++) {
			if ( index == stop_blockindex && k+1 >= offset ) { // 已搜索到最后
				flag = 1;
				break;
			}
				
			state = block[k]; k++;
			memcpy(s, block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
			if ( strcmp(s, lastname) != 0 ) {
				k += 10; // 4+4+2
				continue;
			}
			dir_file = state & 0x01;
			if ( dir_file == 0 ) {
				return NULL; // same path exist;
			}
			
			dir_block = block;
			dir_blockindex = index;
			dir_offset = k + 10; // 位置在尾部
			
			flag = 1;
			break;
		}
		if ( flag ) break;
		
		// get nextblockindex;
		memcpy(b4, block+4, 4);
		index = B4toU32(b4);
		if ( index == 0 ) return NULL; // 到这里说明block有问题
		if ( ! readblock(ffs, index, block) ) return NULL; // 到这里说明block有问题
	}
	
	if ( dir_block == NULL ) { // not exist
		// 创建文件item
		if ( ! do_fopen_createfileitem(ffs, lastname, block_head_index, stop_blockindex, offset, 
			block, &dir_blockindex, &dir_offset) ) {
			return NULL;
		}
		dir_block = block;
	} else {
		// 删除文件内容
		if ( ! do_fopen_cleanfilecontent(ffs, block, dir_blockindex, dir_offset) ) {
			return NULL;
		}
	}

	FFS_FILE *ff;
	ff = (FFS_FILE*)malloc(sizeof(FFS_FILE));
	if ( ff == NULL ) return NULL;
	
	ff->mode = mode;
	
	ff->dir_blockindex = dir_blockindex;
	ff->dir_offset = dir_offset;
	
	ff->file_start_blockindex = 0;
	ff->file_stop_blockindex = 0;
	ff->file_offset = 0;
	
	ff->pos_blockindex = 0;
	ff->pos_offset = 0;
	
	ff->pos = 0;
	
	return ff;
}
static FFS_FILE *do_fopen_a(FileFS *ffs, char *lastname, unsigned char mode, unsigned int block_head_index)
{
	/*
	2 - "a" 	(不可读，可写，无须存在，追加)
	5 - "a+" 	(可读，可写，无须存在，追加)
		if not exist {
			if ( offset == BLOCKSIZE ) {
				new_blockindex = genblock;
				block_stop->nextblockindex = new_blockindex;
				add lastname to block_new;
				start_blockindex = stop_blockindex = offset = 0;
				block_head->stop_blockindex = new_blockindex;
				block_head->offset = 12 + 25;
			} else {
				add lastname to offset;
				start_blockindex = stop_blockindex = offset = 0;
				offset += 25;
			}
			new fp;
			pos_blockindex = 0;
			pos_offsize = 0;
			fp->pos = 0;
			fp->mode = bmode;
		} else {
			new fp;
			pos_blockindex = stop_blockindex;
			pos_offsize = offset;
			while (1) {
				from start_blockindex to stop_blockindex;
				fp->pos += 512;
			}
			fp->pos += offset;
			fp->mode = bmode;
		}
	*/
	unsigned char block[BLOCKSIZE];
	unsigned char state, dir_file;
	unsigned char b4[4], b2[2];
	unsigned int stop_blockindex;
	unsigned short offset;
	int k;
	
	// block_head
	if ( ! readblock(ffs, block_head_index, block) ) return NULL;
	memcpy(b4, block+(12+1+14+4), 4);
	stop_blockindex = B4toU32(b4);
	memcpy(b2, block+(12+1+14+4+4), 2);
	offset = B2toU16(b2);
	
	// 搜索block，检查是否有名称相同的文件
	unsigned char *dir_block = NULL;
	unsigned int dir_blockindex = 0;
	unsigned short dir_offset = 0;
	
	int i;
	unsigned char flag = 0;
	unsigned int index = block_head_index;
	char s[BLOCK_NAME_MAXSIZE+1];
	memset(s, 0, BLOCK_NAME_MAXSIZE+1);
	while (1) {
		// blocksize=512,去掉前置的12byte，一共能放下20个子项目(目录或文件)
		k = BLOCK_HEAD;
		for (i=0; i<BLOCK_ITEM_MAXCOUNT; i++) {
			if ( index == stop_blockindex && k+1 >= offset ) { // 已搜索到最后
				flag = 1;
				break;
			}
				
			state = block[k]; k++;
			memcpy(s, block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
			if ( strcmp(s, lastname) != 0 ) {
				k += 10; // 4+4+2
				continue;
			}
			dir_file = state & 0x01;
			if ( dir_file == 0 ) {
				return NULL; // same path exist;
			}
			
			dir_block = block;
			dir_blockindex = index;
			dir_offset = k + 10; // 位置在尾部
			
			flag = 1;
			break;
		}
		if ( flag ) break;
		
		// get nextblockindex;
		memcpy(b4, block+4, 4);
		index = B4toU32(b4);
		if ( index == 0 ) return NULL; // 到这里说明block有问题
		if ( ! readblock(ffs, index, block) ) return NULL; // 到这里说明block有问题
	}
	
	FFS_FILE *ff;
	if ( dir_block == NULL ) { // not exist
		// 创建文件item
		if ( ! do_fopen_createfileitem(ffs, lastname, block_head_index, stop_blockindex, offset, 
			block, &dir_blockindex, &dir_offset) ) {
			return NULL;
		}
		dir_block = block;
		ff = (FFS_FILE*)malloc(sizeof(FFS_FILE));
		if ( ff == NULL ) return NULL;
		
		ff->mode = mode;
		
		ff->dir_blockindex = dir_blockindex;
		ff->dir_offset = dir_offset;
		
		ff->file_start_blockindex = 0;
		ff->file_stop_blockindex = 0;
		ff->file_offset = 0;
		
		ff->pos_blockindex = 0;
		ff->pos_offset = 0;
		
		ff->pos = 0;
		
		return ff;
	}
	
	// 文件已存在
	//get file_start_blockindex, file_stop_blockindex, file_offset
	unsigned int file_start_blockindex, file_stop_blockindex;
	unsigned short file_offset;
	
	memcpy(b4, dir_block+dir_offset-10, 4);
	file_start_blockindex = B4toU32(b4);
	memcpy(b4, dir_block+dir_offset-6, 4);
	file_stop_blockindex = B4toU32(b4);
	memcpy(b2, dir_block+dir_offset-2, 2);
	file_offset = B2toU16(b2);
	
	unsigned long long pos = 0;
	index = file_start_blockindex;
	while (1) {
		if ( index == file_stop_blockindex ) {
			pos += file_offset - BLOCK_HEAD;
			break;
		}
		
		if ( ! readblock(ffs, index, block) ) return NULL;
		pos += (BLOCKSIZE - BLOCK_HEAD);
		
		memcpy(b4, block+4, 4);
		index = B4toU32(b4);
	}
	
	ff = (FFS_FILE*)malloc(sizeof(FFS_FILE));
	if ( ff == NULL ) return NULL;
	
	ff->mode = mode;
	
	ff->dir_blockindex = dir_blockindex;
	ff->dir_offset = dir_offset;
	
	ff->file_start_blockindex = file_start_blockindex;
	ff->file_stop_blockindex = file_stop_blockindex;
	ff->file_offset = file_offset;
	
	ff->pos_blockindex = file_stop_blockindex;
	ff->pos_offset = file_offset;
	
	ff->pos = pos;
	
	//printf("fopen a, pos_offset:%d\n", ff->pos_offset);
	
	return ff;
}
FFS_FILE *FileFS_fopen(FileFS *ffs, const char *filename, const char *mode)
{
	if ( ffs == NULL ) return NULL;
	if ( ffs->fp == NULL ) return NULL;
	if ( filename == NULL ) return NULL;
	if ( mode == NULL ) return NULL;
	
	/*
	0 - "r" 	(可读，不可写，必须存在)
		if not exist, return NULL;
		new FFS_FILE;
		pos_blockindex = start_blockindex;
		pos_offsize = 0;
		pos = 0;
		mode = bmode;
		return fp;
	1 - "w" 	(不可读，可写，无须存在，清空)
		if exist {
			readblock(file->start_blockindex);
			readblock(file->stop_blockindex);
			stop_block->nextblockindex = new_unused_blockindex;
			ffs->tmp.new_unused_blockindex = start_blockindex;
		} else {
			if ( offset == BLOCKSIZE ) {
				new_blockindex = genblock;
				block_stop->nextblockindex = new_blockindex;
				add lastname to block_new;
				start_blockindex = stop_blockindex = offset = 0;
				block_head->stop_blockindex = new_blockindex;
				block_head->offset = 12 + 25;
			} else {
				add lastname to offset;
				start_blockindex = stop_blockindex = offset = 0;
				offset += 25;
			}
		}
		new fp;
		pos_blockindex = 0;
		pos_offsize = 0;
		fp->pos = 0;
		fp->mode = bmode;
	2 - "a" 	(不可读，可写，无须存在，追加)
		if not exist {
			if ( offset == BLOCKSIZE ) {
				new_blockindex = genblock;
				block_stop->nextblockindex = new_blockindex;
				add lastname to block_new;
				start_blockindex = stop_blockindex = offset = 0;
				block_head->stop_blockindex = new_blockindex;
				block_head->offset = 12 + 25;
			} else {
				add lastname to offset;
				start_blockindex = stop_blockindex = offset = 0;
				offset += 25;
			}
			new fp;
			pos_blockindex = 0;
			pos_offsize = 0;
			fp->pos = 0;
			fp->mode = bmode;
		} else {
			new fp;
			pos_blockindex = stop_blockindex;
			pos_offsize = offset;
			while (1) {
				from start_blockindex to stop_blockindex;
				fp->pos += 512;
			}
			fp->pos += offset;
			fp->mode = bmode;
		}
	3 - "r+" 	(可读，可写，必须存在)
		same 0;
	4 - "w+" 	(可读，可写，无须存在，清空)
		same 1;
	5 - "a+" 	(可读，可写，无须存在，追加)
		same 2;
	*/
	unsigned char bmode;
	if ( strcmp(mode, "r") == 0 ) bmode = 0;
	else if ( strcmp(mode, "w") == 0 ) bmode = 1;
	else if ( strcmp(mode, "a") == 0 ) bmode = 2;
	else if ( strcmp(mode, "r+") == 0 ) bmode = 3;
	else if ( strcmp(mode, "w+") == 0 ) bmode = 4;
	else if ( strcmp(mode, "a+") == 0 ) bmode = 5;
	else return NULL;
	
	int i, start, len = (int)strlen(filename);
	unsigned int blockindex;
	if ( filename[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
	} else if ( filename[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) blockindex = ffs->home_pwd_blockindex; // pwd
		else blockindex = ffs->tmp.home_pwd_blockindex;
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) blockindex = ffs->pwd_blockindex; // pwd
		else blockindex = ffs->tmp.pwd_blockindex;
		start = 0;
	}
	
	char s[BLOCK_NAME_MAXSIZE+2];
	int slen = 0;
	unsigned int index;
	for (i=start; i<len; i++) {
		if ( filename[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) {
				return NULL; // dir not exist
			}
			blockindex = index;
			slen = 0;
			continue;
		}
		s[slen] = filename[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) return NULL; // name to long
	}
	if ( slen == 0 ) return NULL; // just path, no filename
	
	s[slen] = 0;
	if ( slen > BLOCK_NAME_MAXSIZE ) return NULL; // name to long
	
	char lastname[BLOCK_NAME_MAXSIZE+1];
	strcpy(lastname, s);
	if ( strcmp(lastname, ".") == 0 ) return NULL;
	if ( strcmp(lastname, "..") == 0 ) return NULL;
	
	// ============================================
	if ( bmode == 0 || bmode == 3 ) { // "r" "r+"
		return do_fopen_r(ffs, lastname, bmode, blockindex);
	} else if ( bmode == 1 || bmode == 4 ) { // "w" "w+"
		return do_fopen_w(ffs, lastname, bmode, blockindex);
	} else if ( bmode == 2 || bmode == 5 ) { // "a" "a+"
		return do_fopen_a(ffs, lastname, bmode, blockindex);
	}
	
	return NULL;
}

size_t FileFS_fread(FileFS *ffs, void *ptr, size_t size, size_t nmemb, FFS_FILE *stream)
{
	if ( ffs == NULL ) return 0;
	if ( ffs->fp == NULL ) return 0;
	if ( stream == NULL ) return 0;
	if ( stream->mode == 1 || stream->mode == 2 ) return 0; // "w","a"不可读
	
	if ( stream->pos_blockindex == 0 ) return 0; // 空文件
	
	int wannasize = (int)(size * nmemb);
	int k = 0, n;
	unsigned char block[BLOCKSIZE];
	unsigned int blockindex = stream->pos_blockindex, nextindex;
	unsigned char b4[4];

	while (1) {
		if (!readblock(ffs, blockindex, block)) return 0;
		// get nextindex;
		memcpy(b4, block + 4, 4);
		nextindex = B4toU32(b4);
		// printf("posblockindex:%d, pos_offset:%d, next index:%d\n", stream->pos_blockindex, stream->pos_offset, nextindex);

		// printf("fread stop blockindex:%d\n", stream->file_stop_blockindex);
		if ( blockindex == stream->file_stop_blockindex ) {
			// printf("fread start stop blockindex, offset:%d %d %d\n", 
			//	stream->file_start_blockindex, stream->file_stop_blockindex, stream->file_offset);
			
			n = stream->file_offset - stream->pos_offset;
			if ( n <= 0 ) return k;
			if ( wannasize - k < n ) n = wannasize - k;
			memcpy((unsigned char*)ptr + k, block + stream->pos_offset, n);
			k += n;
			stream->pos_blockindex = blockindex;
			stream->pos_offset += n;
			stream->pos += n;
			return k;
		}
		
		n = BLOCKSIZE - stream->pos_offset;
		if ( n <= 0 ) return k;
		if ( wannasize - k < n ) n = wannasize - k;
		memcpy((unsigned char*)ptr + k, block + stream->pos_offset, n);
		k += n;
		stream->pos_blockindex = blockindex;
		stream->pos_offset += n;
		stream->pos += n;
		if ( stream->pos_offset == BLOCKSIZE ) {
			stream->pos_blockindex = nextindex;
			stream->pos_offset = BLOCK_HEAD;
		}
		if ( k >= wannasize ) return k;
		
		// printf("blockindex:%d, nextindex:%d\n", blockindex, nextindex);
		blockindex = nextindex;
		if (nextindex == 0) return k;
	}
	
	return 0;
}

size_t FileFS_fwrite(FileFS *ffs, const void *ptr, size_t size, size_t nmemb, FFS_FILE *stream)
{
	if ( ffs == NULL ) return 0;
	if ( ffs->fp == NULL ) return 0;
	if ( stream == NULL ) return 0;
	if ( stream->mode == 0 ) return 0; // "r"不可写
	
	//printf("mode:%d\n", stream->mode);
	
	int wannasize = (int)(size * nmemb);
	int k, cut = 0, n;
	if ( wannasize <= 0 ) return 0;
	
	unsigned char new_block[BLOCKSIZE];
	unsigned char pos_block[BLOCKSIZE];
	unsigned char dir_block[BLOCKSIZE];
	unsigned int new_blockindex, next_blockindex;
	unsigned char b4[4], b2[2];
	unsigned short offset;
	
	if ( ffs->tmp.state == 0 ) tmpstart(ffs, 1);
	
	if ( stream->pos_blockindex == 0 ) { // 空文件
		new_blockindex = genblockindex(ffs);
		if ( new_blockindex == 0 ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 0;
		}
		memset(pos_block, 0, BLOCKSIZE);
		if ( ! writeblock(ffs, new_blockindex, pos_block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 0;
		}
		
		if ( ! readblock(ffs, stream->dir_blockindex, dir_block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 0;
		}
		U32toB4(new_blockindex, b4);
		memcpy(dir_block + stream->dir_offset-10, b4, 4); // file_start_blockindex
		memcpy(dir_block + stream->dir_offset-6, b4, 4); // file_stop_blockindex
		offset = BLOCK_HEAD;
		U16toB2(offset, b2);
		memcpy(dir_block + stream->dir_offset-2, b2, 2); // file_offset
		stream->file_start_blockindex = new_blockindex;
		stream->file_stop_blockindex = new_blockindex;
		stream->file_offset = BLOCK_HEAD;
		stream->pos_blockindex = new_blockindex;
		stream->pos_offset = BLOCK_HEAD;
		stream->pos = 0;
		if ( ! writeblock(ffs, stream->dir_blockindex, dir_block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 0;
		}
		
		next_blockindex = 0;
	} else {
		//printf("pos_blockindex:%d\n", stream->pos_blockindex);
		if ( ! readblock(ffs, stream->pos_blockindex, pos_block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 0;
		}
		memcpy(b4, pos_block+4, 4);
		next_blockindex = B4toU32(b4);
	}
	
	//printf("pos_blockindex:%d, pos_offset:%d\n", stream->pos_blockindex, stream->pos_offset);
	
	unsigned char hasnewblock = 0;
	while (1) {
		if ( stream->pos_offset == BLOCKSIZE ) {
			if ( next_blockindex == 0 ) {
				new_blockindex = genblockindex(ffs);
				if ( new_blockindex == 0 ) {
					if ( ffs->tmp.state == 1 ) tmpstop(ffs);
					return 0;
				}
				memset(new_block, 0, BLOCKSIZE);
				
				U32toB4(stream->pos_blockindex, b4);
				memcpy(new_block+8, b4, 4); // new_block->prev_blockindex = stream->pos_blockindex;
				U32toB4(new_blockindex, b4);
				memcpy(pos_block+4, b4, 4); // pos_block->next_blockindex = new_blockindex;
				if ( ! writeblock(ffs, stream->pos_blockindex, pos_block) ) {
					if ( ffs->tmp.state == 1 ) tmpstop(ffs);
					return 0;
				}
				
				stream->pos_blockindex = new_blockindex;
				stream->pos_offset = BLOCK_HEAD;
				
				memcpy(pos_block, new_block, BLOCKSIZE);
				
				next_blockindex = 0;
				hasnewblock = 1;
			} else {
				if ( ! readblock(ffs, next_blockindex, pos_block) ) {
					if ( ffs->tmp.state == 1 ) tmpstop(ffs);
					return 0;
				}
				// get next_blockindex;
				memcpy(b4, pos_block+4, 4);
				next_blockindex = B4toU32(b4);
				
				stream->pos_blockindex = next_blockindex;
				stream->pos_offset = BLOCK_HEAD;
			}
		}
		
		k = BLOCKSIZE - stream->pos_offset;
		if ( wannasize - cut <= k ) {
			n = wannasize - cut;
			memcpy(pos_block + stream->pos_offset, (unsigned char*)ptr + cut, n);
			cut += n;
			if ( ! writeblock(ffs, stream->pos_blockindex, pos_block) ) {
				if ( ffs->tmp.state == 1 ) tmpstop(ffs);
				return 0;
			}
			stream->pos_offset += n;
			stream->pos += n;
			
			if ( stream->pos_blockindex == stream->file_stop_blockindex && stream->pos_offset > stream->file_offset) hasnewblock = 1;
			if ( hasnewblock == 1 ) {
				if ( ! readblock(ffs, stream->dir_blockindex, dir_block) ) {
					if ( ffs->tmp.state == 1 ) tmpstop(ffs);
					return 0;
				}
				
				//stream->file_stop_blockindex = stream->pos_blockindex;
				//stream->file_offset = stream->pos_offset;
				
				U32toB4(stream->pos_blockindex, b4);
				memcpy(dir_block+stream->dir_offset-6, b4, 4); // file_stop_blockindex
				U16toB2(stream->pos_offset, b2);
				memcpy(dir_block+stream->dir_offset-2, b2, 2); // file_offset
				if ( ! writeblock(ffs, stream->dir_blockindex, dir_block) ) {
					if ( ffs->tmp.state == 1 ) tmpstop(ffs);
					return 0;
				}
				stream->file_stop_blockindex = stream->pos_blockindex;
				stream->file_offset = stream->pos_offset;
			}
			// commit;
			if ( ffs->tmp.state == 1 ) {
				if ( ! FileFS_commit(ffs) ) {
					return 0;
				}
			}
			return wannasize;
		}
		// wannasize - cut > k
		n = k;
		memcpy(pos_block + stream->pos_offset, (unsigned char*)ptr + cut, n);
		cut += n;
		if ( ! writeblock(ffs, stream->pos_blockindex, pos_block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 0;
		}
		stream->pos_offset += n; // 此时pos_offset==BLOCKSIZE
		stream->pos += n;
		
		if ( wannasize - cut == 0 ) {
			if ( stream->pos_blockindex == stream->file_stop_blockindex && stream->pos_offset > stream->file_offset ) hasnewblock = 1;
			if ( hasnewblock == 1 ) {
				if ( ! readblock(ffs, stream->dir_blockindex, dir_block) ) {
					if ( ffs->tmp.state == 1 ) tmpstop(ffs);
					return 0;
				}
				
				//stream->file_stop_blockindex = stream->pos_blockindex;
				//stream->file_offset = stream->pos_offset;
				
				U32toB4(stream->pos_blockindex, b4);
				memcpy(dir_block+stream->dir_offset-6, b4, 4); // file_stop_blockindex
				U16toB2(stream->pos_offset, b2);
				memcpy(dir_block+stream->dir_offset-2, b2, 2); // file_offset
				if ( ! writeblock(ffs, stream->dir_blockindex, dir_block) ) {
					if ( ffs->tmp.state == 1 ) tmpstop(ffs);
					return 0;
				}
				stream->file_stop_blockindex = stream->pos_blockindex;
				stream->file_offset = stream->pos_offset;
			}
			// commit;
			if ( ffs->tmp.state == 1 ) {
				if ( ! FileFS_commit(ffs) ) {
					return 0;
				}
			}
			return wannasize;
		}
	}
	
	if ( ffs->tmp.state == 1 ) tmpstop(ffs);
	return 0;
}

void FileFS_fclose(FileFS *ffs, FFS_FILE *stream)
{
	if ( ffs == NULL ) return ;
	if ( ffs->fp == NULL ) return;
	if ( stream == NULL ) return;
	
	free(stream);
}

unsigned char FileFS_fseek(FileFS *ffs, FFS_FILE *stream, long long offset, int whence)
{
	if ( ffs == NULL ) return 0;
	if ( ffs->fp == NULL ) return 0;
	if ( stream == NULL ) return 0;
	
	if ( stream->pos_blockindex == 0 ) return 0;
	
	unsigned char block[BLOCKSIZE];
	unsigned char b4[4];
	unsigned int blockindex, next_blockindex, prev_blockindex;
	unsigned short blocksize, pos_offset;
	long long new_offset;
	unsigned long long pos;
	unsigned int index;
	
	if ( whence == FFS_SEEK_CUR ) {
		if ( offset == 0 ) return 1;
		
		if ( offset > 0 ) { // 向文件尾移动
			blockindex = stream->pos_blockindex;
			new_offset = offset;
			pos_offset = stream->pos_offset;
			while (1) {
				if ( blockindex == stream->file_stop_blockindex ) blocksize = stream->file_offset;
				else blocksize = BLOCKSIZE;
				
				if ( blockindex == stream->file_stop_blockindex ) {
					if ( blocksize-pos_offset >= new_offset ) {
						stream->pos_offset += (unsigned short)new_offset;
						stream->pos += new_offset;
					} else {
						stream->pos_offset += (blocksize-pos_offset);
						stream->pos += (blocksize-pos_offset);
					}
					return 1;
				}
				
				stream->pos_offset = BLOCKSIZE;
				stream->pos += BLOCKSIZE - pos_offset;
				new_offset -= (BLOCKSIZE - pos_offset);
				pos_offset = BLOCK_HEAD;
				
				if ( ! readblock(ffs, blockindex, block) ) return 1;
				memcpy(b4, block+8, 4);
				next_blockindex = B4toU32(b4);
				if ( next_blockindex == 0 ) return 1;
				blockindex = next_blockindex;
				
				stream->pos_blockindex = blockindex;
			}
			return 0;
		} else { // 向文件头移动
			blockindex = stream->pos_blockindex;
			new_offset = -offset;
			pos_offset = stream->pos_offset;
			while (1) {
				if ( pos_offset - BLOCK_HEAD >= new_offset ) {
					stream->pos_offset -= (unsigned short)new_offset;
					stream->pos -= new_offset;
					return 1;
				} 

				stream->pos_offset -= pos_offset;
				stream->pos -= pos_offset;
			
				new_offset -= pos_offset;
				pos_offset = BLOCKSIZE;
				
				if ( ! readblock(ffs, blockindex, block) ) return 1;
				memcpy(b4, block+4, 4);
				prev_blockindex = B4toU32(b4);
				if ( prev_blockindex == 0 ) return 1;
				blockindex = prev_blockindex;
				
				stream->pos_blockindex = blockindex;
			}
			return 0;
		}
		return 0;
	}
	
	if ( whence == FFS_SEEK_END ) {
		pos = stream->pos - (stream->pos_offset - BLOCK_HEAD);
		index = stream->pos_blockindex;
		while (1) {
			if ( index == stream->file_stop_blockindex ) {
				pos += stream->file_offset - BLOCK_HEAD;
				break;
			}
		
			if ( ! readblock(ffs, index, block) ) return 0;
			pos += (BLOCKSIZE - BLOCK_HEAD);
		
			memcpy(b4, block+4, 4);
			index = B4toU32(b4);
		}
		// =======================
		stream->pos_blockindex = stream->file_stop_blockindex;
		stream->pos_offset = stream->file_offset;
		stream->pos = pos;
		// =======================
		if ( offset == 0 ) return 1;
		
		if ( offset < 0 ) { // 向文件头移动
			blockindex = stream->pos_blockindex;
			new_offset = -offset;
			pos_offset = stream->pos_offset;
			while (1) {
				if ( pos_offset - BLOCK_HEAD >= new_offset ) {
					stream->pos_offset -= (unsigned short)new_offset;
					stream->pos -= new_offset;
					return 1;
				} 

				stream->pos_offset -= pos_offset;
				stream->pos -= pos_offset;
			
				new_offset -= pos_offset;
				pos_offset = BLOCKSIZE;
				
				if ( ! readblock(ffs, blockindex, block) ) return 1;
				memcpy(b4, block+4, 4);
				prev_blockindex = B4toU32(b4);
				if ( prev_blockindex == 0 ) return 1;
				blockindex = prev_blockindex;
				
				stream->pos_blockindex = blockindex;
			}
			return 0;
		}
		return 0;
	}
	
	if ( whence == FFS_SEEK_SET ) {
		stream->pos_blockindex = stream->file_start_blockindex;
		stream->pos_offset = BLOCK_HEAD;
		stream->pos = 0;
		
		if ( offset == 0 ) return 1;
		
		if ( offset > 0 ) { // 向文件尾移动
			blockindex = stream->pos_blockindex;
			new_offset = offset;
			pos_offset = stream->pos_offset;
			while (1) {
				if ( blockindex == stream->file_stop_blockindex ) blocksize = stream->file_offset;
				else blocksize = BLOCKSIZE;
				
				if ( blockindex == stream->file_stop_blockindex ) {
					if ( blocksize-pos_offset >= new_offset ) {
						stream->pos_offset += (unsigned short)new_offset;
						stream->pos += new_offset;
					} else {
						stream->pos_offset += (blocksize-pos_offset);
						stream->pos += (blocksize-pos_offset);
					}
					return 1;
				}
				
				stream->pos_offset = BLOCKSIZE;
				stream->pos += BLOCKSIZE - pos_offset;
				new_offset -= (BLOCKSIZE - pos_offset);
				pos_offset = BLOCK_HEAD;
				
				if ( ! readblock(ffs, blockindex, block) ) return 1;
				memcpy(b4, block+8, 4);
				next_blockindex = B4toU32(b4);
				if ( next_blockindex == 0 ) return 1;
				blockindex = next_blockindex;
				
				stream->pos_blockindex = blockindex;
			}
			return 0;
		}
		// 向文件头移动
		return 0;
	}
	
	return 0;
}
unsigned long long FileFS_ftell(FileFS *ffs, FFS_FILE *stream)
{
	if ( ffs == NULL ) return 0;
	if ( ffs->fp == NULL ) return 0;
	if ( stream == NULL ) return 0;
	
	return stream->pos;
}
void FileFS_rewind(FileFS *ffs, FFS_FILE *stream)
{
	FileFS_fseek(ffs, stream, 0, FFS_SEEK_SET);
}

// =================================
// return:
// 0:not exist
// 1:is file
// 2:is dir
static unsigned char FileFS_stat(FileFS *ffs, const char *name)
{
	if ( name == NULL ) return 0;
	if ( ffs == NULL ) return 0;
	if ( ffs->fp == NULL ) return 0;
	
	unsigned char block[BLOCKSIZE];
	
	int i, start, len = (int)strlen(name);
	unsigned int blockindex;
	if ( name[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
	} else if ( name[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) blockindex = ffs->home_pwd_blockindex; // pwd
		else blockindex = ffs->tmp.home_pwd_blockindex;
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) blockindex = ffs->pwd_blockindex; // pwd
		else blockindex = ffs->tmp.pwd_blockindex;
		start = 0;
	}
	
	char s[BLOCK_NAME_MAXSIZE + 2];
	int slen = 0;
	unsigned int index;
	memset(s, 0, BLOCK_NAME_MAXSIZE+2);
	for (i=start; i<len; i++) {
		if ( name[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) return 0;
			blockindex = index;
			slen = 0;
			continue;
		}
		s[slen] = name[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 0;
	}
	if ( slen == 0 ) return 2; // dir, name作为目录形式全部搜索完毕
	
	// slen > 0
	s[slen] = 0; // 此时的s是形似xx/yy/zz/abc最后的abc，可能是文件，也可能是目录
	if ( slen > BLOCK_NAME_MAXSIZE ) return 0;
	char lastname[BLOCK_NAME_MAXSIZE+1];
	strcpy(lastname, s);
	
	// ===============================
	unsigned char state, dir_file;
	unsigned char b4[4], b2[2];
	unsigned int stop_blockindex;
	unsigned short offset;
	int k;
	
	if ( ! readblock(ffs, blockindex, block) ) return 0;
	memcpy(b4, block+(12+1+14+4), 4);
	stop_blockindex = B4toU32(b4);
	memcpy(b2, block+(12+1+14+4+4), 2);
	offset = B2toU16(b2);
	
	index = blockindex;
	while (1) {
		// blocksize=512,去掉前置的12byte，一共能放下20个子项目(目录或文件)
		k = BLOCK_HEAD;
		for (i=0; i<BLOCK_ITEM_MAXCOUNT; i++) {
			if ( index == stop_blockindex && k+1 >= offset ) return 0; // 已搜索到最后
			state = block[k]; k++;
			memcpy(s, block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
			if ( strcmp(s, lastname) != 0 ) {
				k += 10; // 4+4+2
				continue;
			}
			dir_file = state & 0x01;
			if ( dir_file == 0 ) return 2; // dir;
			return 1; // file;
		}
		
		// get nextblockindex;
		memcpy(b4, block+4, 4);
		index = B4toU32(b4);
		if ( index == 0 ) return 0;
		if ( ! readblock(ffs, index, block) ) return 0;
	}
	
	return 0; // 正常情况下不会执行到这里
}

unsigned char FileFS_file_exist(FileFS *ffs, const char *filename)
{
	unsigned char r = FileFS_stat(ffs, filename);
	if ( r != 1 ) return 0;
	return 1;
}

unsigned char FileFS_dir_exist(FileFS *ffs, const char *pathname)
{
	unsigned char r = FileFS_stat(ffs, pathname);
	if ( r != 2 ) return 0;
	return 1;
}

// return: 0-ok,1-gen err,2-file not exist,3-dir not existed,4-name>limit(14byte),5-name format err
int FileFS_remove(FileFS *ffs, const char *filename)
{
	if ( ffs == NULL ) return 1;
	if ( ffs->fp == NULL ) return 1;
	if ( filename == NULL ) return 1;
	
	// ===================================
	// 检查路径
	int i, start, len = (int)strlen(filename);
	if ( filename[len-1] == '/' ) return 5; // ...xyz/，最后一位是'/'，表示不是文件
	unsigned int blockindex;
	if ( filename[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
	} else if ( filename[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) blockindex = ffs->home_pwd_blockindex; // pwd
		else blockindex = ffs->tmp.home_pwd_blockindex;
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) blockindex = ffs->pwd_blockindex; // pwd
		else blockindex = ffs->tmp.pwd_blockindex;
		start = 0;
	}
	
	char s[BLOCK_NAME_MAXSIZE+2];
	int slen = 0;
	unsigned int index;
	for (i=start; i<len; i++) {
		if ( filename[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) {
				return 3; // dir not exist
			}
			blockindex = index;
			slen = 0;
			continue;
		}
		s[slen] = filename[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 4; // name to long
	}
	if ( slen == 0 ) return 2; // just path, no filename
	
	s[slen] = 0;
	if ( slen > BLOCK_NAME_MAXSIZE ) return 4; // name to long
	
	char lastname[BLOCK_NAME_MAXSIZE+1];
	strcpy(lastname, s);
	if ( strcmp(lastname, ".") == 0 ) return 5; // format err
	if ( strcmp(lastname, "..") == 0 ) return 5; // format err
	
	// ============================================
	// 检查文件是否存在
	BlockArray ba[4];
	int ba_used = 0;
	unsigned char *block_head, *block_last, *block_item, *block_prev;
	unsigned int block_item_index, block_last_index, block_prev_index, block_head_index;
	for (i=0; i<4; i++) ba[i].active = 0;
	
	unsigned char block[BLOCKSIZE];
	unsigned char state, dir_file;
	unsigned char b4[4], b2[2];
	unsigned int stop_blockindex;
	unsigned short offset;
	int k;
	
	// block_head
	if ( ! readblock(ffs, blockindex, block) ) return 1;
	memcpy(ba[0].block, block, BLOCKSIZE);
	ba[0].blockindex = blockindex;
	ba[0].active = 1;
	block_head = ba[0].block;
	block_head_index = blockindex;
	ba_used = 1;
	
	memcpy(b4, block_head+(12+1+14+4), 4);
	stop_blockindex = B4toU32(b4);
	memcpy(b2, block_head+(12+1+14+4+4), 2);
	offset = B2toU16(b2);
	
	// block_last
	if ( stop_blockindex == block_head_index ) {
		block_last = block_head;
		block_last_index = block_head_index;
	} else {
		if ( ! readblock(ffs, stop_blockindex, ba[1].block) ) return 1;
		ba[1].blockindex = stop_blockindex;
		ba[1].active = 1;
		block_last = ba[1].block;
		block_last_index = stop_blockindex;
		ba_used++;
	}
	
	// 搜索block，检查是否有名称相同的目录或文件
	unsigned int file_start_blockindex, file_stop_blockindex;
	unsigned short item_offset = 0;
	
	unsigned char flag = 0, u;
	index = block_head_index;
	while (1) {
		// blocksize=512,去掉前置的12byte，一共能放下20个子项目(目录或文件)
		k = BLOCK_HEAD;
		for (i=0; i<BLOCK_ITEM_MAXCOUNT; i++) {
			// printf("index:%d, stop_blockindex:%d, k=%d, offset:%d\n", index, stop_blockindex, k, offset);
			if ( index == stop_blockindex && k+1 >= offset ) { // 已搜索到最后
				return 2; // file not exist
			}
			state = block[k]; k++;
			memcpy(s, block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
			if ( strcmp(s, lastname) != 0 ) {
				k += 10; // 4+4+2
				continue;
			}
			//printf("state:%d\n", state);
			dir_file = state & 0x01;
			if ( dir_file == 0 ) {
				return 2; // same path exist;
			}
			
			// 读取文件参数
			memcpy(b4, block+k, 4); file_start_blockindex = B4toU32(b4);
			memcpy(b4, block+k+4, 4); file_stop_blockindex = B4toU32(b4);
			
			item_offset = k + 10; // block item的尾部位置
			
			// block_item
			u = 0;
			for (i=0; i<ba_used; i++) {
				if ( ! ba[i].active ) continue;
				if ( ba[i].blockindex == index ) {
					block_item = ba[i].block;
					block_item_index = index;
					u = 1;
					break;
				}
			}
			if ( !u ) {
				memcpy(ba[ba_used].block, block, BLOCKSIZE);
				ba[ba_used].blockindex = index;
				ba[ba_used].active = 1;
				block_item = ba[ba_used].block;
				block_item_index = index;
				ba_used++;
			}
			
			flag = 1;
			break;
		}
		if ( flag ) break;
		
		// get nextblockindex;
		memcpy(b4, block+4, 4);
		index = B4toU32(b4);
		if ( index == 0 ) return 1; // 到这里说明block有问题
		if ( ! readblock(ffs, index, block) ) return 1; // 到这里说明block有问题
	}
	
	// printf("head:%d, item:%d, last:%d, item_offset:%d\n", block_head_index, block_item_index, block_last_index, item_offset);
	// =======================
	// 正式开始删除目录项
	if ( ffs->tmp.state == 0 ) tmpstart(ffs, 1);

	// 删除文件内容
	if ( file_start_blockindex > 0 ) { // 文件有内容
		// file block_stop
		unsigned char file_block_stop[BLOCKSIZE];
		if ( ! readblock(ffs, file_stop_blockindex, file_block_stop) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		
		U32toB4(ffs->tmp.new_unused_blockhead, b4);
		memcpy(file_block_stop + 4, b4, 4);
		ffs->tmp.new_unused_blockhead = file_start_blockindex;
		
		if ( ! writeblock(ffs, file_stop_blockindex, file_block_stop) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
	}
	
	/*
	if ( item is not offset ) {
		get offset->item;
		copy item;
	}
	1.block_item
	*/
	if ( block_item_index != stop_blockindex || item_offset != offset ) {
		memcpy(block_item + item_offset - 25, block_last + offset - 25, 25);
	}

	/*
	offset -= 25;
	2.block_last
	*/
	offset -= 25;
	U16toB2(offset, b2);
	memcpy(block_head + BLOCK_OFFSET, b2, 2);
	
	/*
	if ( offset < 25 ) { // 整个block都已清空
		get prevblockindex;
		remove block_last;
		prevblock->next_blockindex = 0;
		stop_blockindex = prev_block_index;
		offset = BLOCKSIZE;
		3.block_prev
	}
	4.block_head
	*/
	if ( offset < 25 ) {
		memcpy(b4, block_last+8, 4);
		block_prev_index = B4toU32(b4);
		// printf("block prev index:%d\n", block_prev_index);
		
		removeblock(ffs, block_last_index);
		k = -1;
		for (i=0; i<ba_used; i++) {
			if ( ! ba[i].active ) continue;
			if ( ba[i].blockindex == block_last_index ) {
				ba[i].active = 0;
				k = i;
				break;
			}
		}
		if ( k < 0 ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		
		// block_prev
		u = 0;
		for (i=0; i<ba_used; i++) {
			if ( ! ba[i].active ) continue;
			if ( ba[i].blockindex == block_prev_index ) {
				block_prev = ba[i].block;
				u = 1;
				break;
			}
		}
		if ( !u ) {
			if ( ! readblock(ffs, block_prev_index, block) ) {
				if ( ffs->tmp.state == 1 ) tmpstop(ffs);
				return 1; // 到这里说明block有问题
			}
			
			memcpy(ba[k].block, block, BLOCKSIZE);
			ba[k].blockindex = block_prev_index;
			ba[k].active = 1;
			block_prev = ba[k].block;
		}
		
		// prevblock->next_blockindex = 0;
		memset(block_prev + 4, 0, 4);
		
		U32toB4(block_prev_index, b4);
		memcpy(block_head + BLOCK_STOP_BLOCKINDEX, b4, 4);
		offset = BLOCKSIZE;
		U16toB2(offset, b2);
		memcpy(block_head + BLOCK_OFFSET, b2, 2);
	}
	
	for (i=0; i<ba_used; i++) {
		// printf("%d: write blockindex:%d\n", i, ba[i].blockindex);
		if ( ! ba[i].active ) continue;
		/*
		for (k=0; k<BLOCKSIZE; k++) {
			printf("%x ", ba[i].block[k]);
		}
		printf("\n");
		*/
		if ( ! writeblock(ffs, ba[i].blockindex, ba[i].block) ) {
			// printf("write block err %d\n", i);
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
	}
	
	if ( ffs->tmp.state == 1 ) {
		if ( ! FileFS_commit(ffs) ) {
			return 1;
		}
	}
	
	// ============================================
	return 0;
}

// =====================
static int do_rename(FileFS *ffs, char *old_lastname, unsigned int old_blockindex, unsigned char old_type_dir, 
	char *new_lastname, unsigned int new_blockindex, unsigned char new_type_dir)
{
	int i;
	unsigned int index;
	char s[BLOCK_NAME_MAXSIZE+2];
	
	// ===================================
	unsigned char b4[4], b2[2];
	int k;
	// ===================================
	// old lastname exist
	BlockArray old_ba[4];
	int old_ba_used = 0;
	unsigned char *old_block_head, *old_block_last, *old_block_item, *old_block_prev;
	unsigned int old_block_item_index, old_block_last_index, old_block_prev_index, old_block_head_index;
	for (i=0; i<4; i++) old_ba[i].active = 0;
	
	unsigned char old_block[BLOCKSIZE];
	unsigned char state, old_dir_file;
	unsigned int old_stop_blockindex;
	unsigned short old_offset;
	
	// block_head
	if ( ! readblock(ffs, old_blockindex, old_block) ) return 1;
	memcpy(old_ba[0].block, old_block, BLOCKSIZE);
	old_ba[0].blockindex = old_blockindex;
	old_ba[0].active = 1;
	old_block_head = old_ba[0].block;
	old_block_head_index = old_blockindex;
	old_ba_used = 1;
	
	memcpy(b4, old_block_head+(12+1+14+4), 4);
	old_stop_blockindex = B4toU32(b4);
	memcpy(b2, old_block_head+(12+1+14+4+4), 2);
	old_offset = B2toU16(b2);
	
	// block_last
	if ( old_stop_blockindex == old_block_head_index ) {
		old_block_last = old_block_head;
		old_block_last_index = old_block_head_index;
	} else {
		if ( ! readblock(ffs, old_stop_blockindex, old_ba[1].block) ) return 1;
		old_ba[1].blockindex = old_stop_blockindex;
		old_ba[1].active = 1;
		old_block_last = old_ba[1].block;
		old_block_last_index = old_stop_blockindex;
		old_ba_used++;
	}
	
	// 搜索block，检查是否有名称相同的目录或文件
	unsigned short old_item_offset = 0;
	unsigned char flag = 0, u;
	index = old_block_head_index;
	while (1) {
		// blocksize=512,去掉前置的12byte，一共能放下20个子项目(目录或文件)
		k = BLOCK_HEAD;
		for (i=0; i<BLOCK_ITEM_MAXCOUNT; i++) {
			if ( index == old_stop_blockindex && k+1 >= old_offset ) { // 已搜索到最后
				return 4; // old lastname item not exist
			}
			state = old_block[k]; k++;
			memcpy(s, old_block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
			if ( strcmp(s, old_lastname) != 0 ) {
				k += 10; // 4+4+2
				continue;
			}
			//printf("state:%d\n", state);
			old_dir_file = state & 0x01; // 0-dir,1-file
			if ( old_type_dir == 1 && old_dir_file == 1 ) return 2; // old_name指定为目录，读取出来为文件，前后不一致
			if ( new_type_dir == 1 && old_dir_file == 1 ) return 6; // new_name指定为目录，old_name为文件，格式不匹配
			old_item_offset = k + 10; // block item的尾部位置
			
			// block_item
			u = 0;
			for (i=0; i<old_ba_used; i++) {
				if ( old_ba[i].blockindex == index ) {
					old_block_item = old_ba[i].block;
					old_block_item_index = index;
					u = 1;
					break;
				}
			}
			if ( !u ) {
				memcpy(old_ba[old_ba_used].block, old_block, BLOCKSIZE);
				old_ba[old_ba_used].blockindex = index;
				old_ba[old_ba_used].active = 1;
				old_block_item = old_ba[old_ba_used].block;
				old_block_item_index = index;
				old_ba_used++;
			}
			
			flag = 1;
			break;
		}
		if ( flag ) break;
		
		// get nextblockindex;
		memcpy(b4, old_block+4, 4);
		index = B4toU32(b4);
		if ( index == 0 ) return 1; // 到这里说明block有问题
		if ( ! readblock(ffs, index, old_block) ) return 1; // 到这里说明block有问题
	}
	
	// ===================================
	// new lastname no exist
	BlockArray new_ba[2];
	int new_ba_used = 0;
	unsigned char *new_block_head, *new_block_last;
	unsigned int new_block_head_index, new_block_last_index;
	for (i=0; i<2; i++) new_ba[i].active = 0;
	
	unsigned char new_block[BLOCKSIZE];
	unsigned int new_stop_blockindex;
	unsigned short new_offset;
	
	// block_head
	if ( ! readblock(ffs, new_blockindex, new_block) ) return 1;
	memcpy(new_ba[0].block, new_block, BLOCKSIZE);
	new_ba[0].blockindex = new_blockindex;
	new_ba[0].active = 1;
	new_block_head = new_ba[0].block;
	new_block_head_index = new_blockindex;
	new_ba_used = 1;
	
	memcpy(b4, new_block_head+(12+1+14+4), 4);
	new_stop_blockindex = B4toU32(b4);
	memcpy(b2, new_block_head+(12+1+14+4+4), 2);
	new_offset = B2toU16(b2);
	
	// block_last
	if ( new_stop_blockindex == new_block_head_index ) {
		new_block_last = new_block_head;
		new_block_last_index = new_block_head_index;
	} else {
		if ( ! readblock(ffs, new_stop_blockindex, new_ba[1].block) ) return 1;
		new_ba[1].blockindex = new_stop_blockindex;
		new_ba[1].active = 1;
		new_block_last = new_ba[1].block;
		new_block_last_index = new_stop_blockindex;
		new_ba_used++;
	}
	
	// 搜索block，检查是否有名称相同的目录或文件
	flag = 0;
	index = new_block_head_index;
	while (1) {
		// blocksize=512,去掉前置的12byte，一共能放下20个子项目(目录或文件)
		k = BLOCK_HEAD;
		for (i=0; i<BLOCK_ITEM_MAXCOUNT; i++) {
			// printf("index:%d, new_stop_blockindex:%d, k=%d, offset:%d\n", index, new_stop_blockindex, k, offset);
			if ( index == new_stop_blockindex && k+1 >= new_offset ) { // 已搜索到最后
				flag = 1;
				break; // new lastname item not exist
			}
			k++; // 跳过state
			memcpy(s, new_block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
			if ( strcmp(s, new_lastname) != 0 ) {
				k += 10; // 4+4+2
				continue;
			}
			return 5; // new name already exist
		}
		if ( flag ) break;
		
		// get nextblockindex;
		memcpy(b4, new_block+4, 4);
		index = B4toU32(b4);
		if ( index == 0 ) return 1; // 到这里说明block有问题
		if ( ! readblock(ffs, index, new_block) ) return 1; // 到这里说明block有问题
	}
	
	// =======================================
	// 此时old_name存在，new_name不存在
	// =======================================
	// old和new在同一个目录中，只需更换lastname即可
	if ( old_block_head_index == new_block_head_index ) {
		memcpy(old_block_item + old_item_offset - 10 - 14, new_lastname, BLOCK_NAME_MAXSIZE);
		
		if ( ffs->tmp.state == 0 ) tmpstart(ffs, 1);
		if ( ! writeblock(ffs, old_block_item_index, old_block_item) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		if ( ffs->tmp.state == 1 ) {
			if ( ! FileFS_commit(ffs) ) {
				return 1;
			}
		}
		return 0;
	}
	
	// =======================================
	// old和new不在同一个目录中
	if ( ffs->tmp.state == 0 ) tmpstart(ffs, 1);
	
	// 若移动的是目录，需将(new_item指向的目录块)->..->start_blockindex = new_block_head_index
	unsigned int path_blockindex;
	unsigned char path_block[BLOCKSIZE];
	if ( old_dir_file == 0 ) {
		memcpy(b4, old_block_item + old_item_offset - 10, 4);
		path_blockindex = B4toU32(b4);
		if ( ! readblock(ffs, path_blockindex, path_block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		U32toB4(new_block_head_index, b4);
		memcpy(path_block + BLOCK_HEAD + 25 + 1 + 14, b4, 4);
		if ( ! writeblock(ffs, path_blockindex, path_block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
	}
	
	// == 在new_block中创建一个新的item，将old_item复制过来
	unsigned int blockindex_2;
	unsigned char block_2[BLOCKSIZE];
	
	if ( new_offset < BLOCKSIZE ) { // 最后一个block未填满		
		// 向new_block_last写入lastname目录项
		memcpy(new_block_last + new_offset, old_block_item + old_item_offset - 25, 25);
		
		new_offset += 25;
		U16toB2(new_offset, b2);
		memcpy(new_block_head + BLOCK_OFFSET, b2, 2);
	} else { // 最后一个block已填满
		// 创建存储lastname的目录延伸块
		// gen block_2 for lastname;
		// block_2->prevblockindex = cur_blockindex;
		// write;
		blockindex_2 = genblockindex(ffs);
		if ( blockindex_2 == 0 ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}		
		memset(block_2, 0, BLOCKSIZE);
		// prevblockindex
		U32toB4(new_block_last_index, b4);
		memcpy(block_2+8, b4, 4);
		// copy item
		memcpy(block_2 + BLOCK_HEAD, old_block_item + old_item_offset - 25, 25);
		if ( ! writeblock(ffs, blockindex_2, block_2) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		
		// new_block_last->nextblockindex = blockindex_2;
		U32toB4(blockindex_2, b4);
		memcpy(new_block_last + 4, b4, 4);
		
		new_offset = BLOCK_HEAD + 25;
		U16toB2(new_offset, b2);
		memcpy(new_block_head + BLOCK_OFFSET, b2, 2);
		U32toB4(blockindex_2, b4);
		memcpy(new_block_head + BLOCK_STOP_BLOCKINDEX, b4, 4);
	}
	for (i=0; i<new_ba_used; i++) {
		if ( ! new_ba[i].active ) continue;
		if ( ! writeblock(ffs, new_ba[i].blockindex, new_ba[i].block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
	}
	
	// == 删除old_item
	if ( old_block_item_index != old_stop_blockindex || old_item_offset != old_offset ) { // 要删除的item不是最后一项
		memcpy(old_block_item + old_item_offset - 25, old_block_last + old_offset - 25, 25);
	}

	old_offset -= 25;
	U16toB2(old_offset, b2);
	memcpy(old_block_head + BLOCK_OFFSET, b2, 2);
	
	if ( old_offset < 25 ) { // 整个old_block_last都已清空
		memcpy(b4, old_block_last+8, 4);
		old_block_prev_index = B4toU32(b4);
		
		removeblock(ffs, old_block_last_index);
		k = -1;
		for (i=0; i<old_ba_used; i++) {
			if ( old_ba[i].blockindex == old_block_last_index ) {
				old_ba[i].active = 0;
				k = i;
				break;
			}
		}
		if ( k < 0 ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		
		// block_prev
		u = 0;
		for (i=0; i<old_ba_used; i++) {
			if ( old_ba[i].blockindex == old_block_prev_index ) {
				old_block_prev = old_ba[i].block;
				u = 1;
				break;
			}
		}
		if ( !u ) {
			if ( ! readblock(ffs, old_block_prev_index, old_block) ) {
				if ( ffs->tmp.state == 1 ) tmpstop(ffs);
				return 1; // 到这里说明block有问题
			}
			
			memcpy(old_ba[k].block, old_block, BLOCKSIZE);
			old_ba[k].blockindex = old_block_prev_index;
			old_ba[k].active = 1;
			old_block_prev = old_ba[k].block;
		}
		
		// prevblock->next_blockindex = 0;
		memset(old_block_prev + 4, 0, 4);
		
		U32toB4(old_block_prev_index, b4);
		memcpy(old_block_head + BLOCK_STOP_BLOCKINDEX, b4, 4);
		old_offset = BLOCKSIZE;
		U16toB2(old_offset, b2);
		memcpy(old_block_head + BLOCK_OFFSET, b2, 2);
	}
	
	for (i=0; i<old_ba_used; i++) {
		if ( ! old_ba[i].active ) continue;
		if ( ! writeblock(ffs, old_ba[i].blockindex, old_ba[i].block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
	}
	
	// commit
	if ( ffs->tmp.state == 1 ) {
		if ( ! FileFS_commit(ffs) ) {
			return 1;
		}
	}

	return 0;
}

// return: 0:ok,1-err,2-old name format err,3-new name format err,4-old name not exist,5-new name exist, 6-old new format not match
int FileFS_rename(FileFS *ffs, const char *old_name, const char *new_name)
{
	if ( ffs == NULL ) return 1;
	if ( ffs->fp == NULL ) return 1;
	if ( old_name == NULL ) return 1;
	if ( new_name == NULL ) return 1;
	
	// =================================
	// check old name
	int i, start, len = (int)strlen(old_name);
	unsigned int blockindex;
	if ( old_name[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
	} else if ( old_name[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) blockindex = ffs->home_pwd_blockindex; // pwd
		else blockindex = ffs->tmp.home_pwd_blockindex;
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) blockindex = ffs->pwd_blockindex; // pwd
		else blockindex = ffs->tmp.pwd_blockindex;
		start = 0;
	}
	
	char s[BLOCK_NAME_MAXSIZE+2];
	int slen = 0;
	unsigned int index;
	memset(s, 0, BLOCK_NAME_MAXSIZE+2);
	for (i=start; i<len; i++) {
		if ( old_name[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			if ( i == len - 1 ) break; // 留下最后一个s不判断
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) {
				return 2; // dir not exist
			}
			blockindex = index;
			slen = 0;
			continue;
		}
		s[slen] = old_name[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 2; // name to long
	}
	if ( slen > 0 ) {
		s[slen] = 0;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 2; // name to long
	}
	
	char old_lastname[BLOCK_NAME_MAXSIZE+1];
	strcpy(old_lastname, s);
	if ( strcmp(old_lastname, ".") == 0 ) return 2;
	if ( strcmp(old_lastname, "..") == 0 ) return 2;
	unsigned int old_blockindex = blockindex;
	unsigned char old_type_dir = 0;
	if ( old_name[len-1] == '/' ) old_type_dir = 1; // 尾部有'/'，说明old_name只能是目录
	
	// ========================================
	// check new name
	len = (int)strlen(new_name);
	if ( new_name[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
	} else if ( new_name[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) blockindex = ffs->home_pwd_blockindex; // pwd
		else blockindex = ffs->tmp.home_pwd_blockindex;
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) blockindex = ffs->pwd_blockindex; // pwd
		else blockindex = ffs->tmp.pwd_blockindex;
		start = 0;
	}
	
	slen = 0;
	memset(s, 0, BLOCK_NAME_MAXSIZE+2);
	for (i=start; i<len; i++) {
		if ( new_name[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			if ( i == len - 1 ) break; // 留下最后一个s不判断
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) {
				return 3; // dir not exist
			}
			blockindex = index;
			slen = 0;
			continue;
		}
		s[slen] = new_name[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 3; // name to long
	}
	if ( slen > 0 ) {
		s[slen] = 0;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 3; // name to long
	}
	
	char new_lastname[BLOCK_NAME_MAXSIZE+1];
	strcpy(new_lastname, s);
	if ( strcmp(new_lastname, ".") == 0 ) return 3;
	if ( strcmp(new_lastname, "..") == 0 ) return 3;
	unsigned int new_blockindex = blockindex;
	unsigned char new_type_dir = 0;
	if ( new_name[len-1] == '/' ) new_type_dir = 1; // 尾部有'/'，说明new_name只能是目录
	
	return do_rename(ffs, old_lastname, old_blockindex, old_type_dir, new_lastname, new_blockindex, new_type_dir);
}

int FileFS_move(FileFS *ffs, const char *from_name, const char *to_path)
{
	if ( ffs == NULL ) return 1;
	if ( ffs->fp == NULL ) return 1;
	if ( from_name == NULL ) return 1;
	if ( to_path == NULL ) return 1;
	
	// =================================
	// check from name
	int i, start, len = (int)strlen(from_name);
	unsigned int blockindex;
	if ( from_name[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
	} else if ( from_name[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) blockindex = ffs->home_pwd_blockindex; // pwd
		else blockindex = ffs->tmp.home_pwd_blockindex;
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) blockindex = ffs->pwd_blockindex; // pwd
		else blockindex = ffs->tmp.pwd_blockindex;
		start = 0;
	}
	
	char s[BLOCK_NAME_MAXSIZE+2];
	int slen = 0;
	unsigned int index;
	memset(s, 0, BLOCK_NAME_MAXSIZE+2);
	for (i=start; i<len; i++) {
		if ( from_name[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			if ( i == len - 1 ) break; // 留下最后一个s不判断
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) {
				return 2; // dir not exist
			}
			blockindex = index;
			slen = 0;
			continue;
		}
		s[slen] = from_name[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 2; // name to long
	}
	if ( slen > 0 ) {
		s[slen] = 0;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 2; // name to long
	}
	
	char from_lastname[BLOCK_NAME_MAXSIZE+1];
	strcpy(from_lastname, s);
	if ( strcmp(from_lastname, ".") == 0 ) return 2;
	if ( strcmp(from_lastname, "..") == 0 ) return 2;
	unsigned int from_blockindex = blockindex;
	unsigned char from_type_dir = 0;
	if ( from_name[len-1] == '/' ) from_type_dir = 1; // 尾部有'/'，说明old_name只能是目录
	
	// ========================================
	// check to path
	len = (int)strlen(to_path);
	if ( to_path[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
	} else if ( to_path[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) blockindex = ffs->home_pwd_blockindex; // pwd
		else blockindex = ffs->tmp.home_pwd_blockindex;
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) blockindex = ffs->pwd_blockindex; // pwd
		else blockindex = ffs->tmp.pwd_blockindex;
		start = 0;
	}
	
	slen = 0;
	memset(s, 0, BLOCK_NAME_MAXSIZE+2);
	for (i=start; i<len; i++) {
		if ( to_path[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) {
				return 3; // dir not exist
			}
			blockindex = index;
			slen = 0;
			continue;
		}
		s[slen] = to_path[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 3; // name to long
	}
	if ( slen > 0 ) {
		s[slen] = 0;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 3; // name to long
		index = findPathBlockindex(ffs, blockindex, s);
		if ( index < 1 ) {
			return 3; // dir not exist
		}
		blockindex = index;
	}
	
	char to_lastname[BLOCK_NAME_MAXSIZE+1];
	strcpy(to_lastname, from_lastname);
	unsigned int to_blockindex = blockindex;
	unsigned char to_type_dir = from_type_dir;
	
	return do_rename(ffs, from_lastname, from_blockindex, from_type_dir, to_lastname, to_blockindex, to_type_dir);
}

// =============================================
int FileFS_copy(FileFS *ffs, const char *from_filename, const char *to_filename)
{
	if ( ffs == NULL ) return 1;
	if ( ffs->fp == NULL ) return 1;
	if ( from_filename == NULL ) return 1;
	if ( to_filename == NULL ) return 1;
	
	// ===========================
	// check from path
	int i, start, len = (int)strlen(from_filename);
	unsigned int blockindex;
	if ( from_filename[len-1] == '/' ) return 2; // 尾部有'/'，说明from_filename是目录
	if ( from_filename[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
	} else if ( from_filename[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) blockindex = ffs->home_pwd_blockindex; // pwd
		else blockindex = ffs->tmp.home_pwd_blockindex;
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) blockindex = ffs->pwd_blockindex; // pwd
		else blockindex = ffs->tmp.pwd_blockindex;
		start = 0;
	}
	
	char s[BLOCK_NAME_MAXSIZE+2];
	int slen = 0;
	unsigned int index;
	memset(s, 0, BLOCK_NAME_MAXSIZE+2);
	for (i=start; i<len; i++) {
		if ( from_filename[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			if ( i == len - 1 ) break; // 留下最后一个s不判断
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) {
				return 2; // dir not exist
			}
			blockindex = index;
			slen = 0;
			continue;
		}
		s[slen] = from_filename[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 2; // name to long
	}
	if ( slen > 0 ) {
		s[slen] = 0;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 2; // name to long
	}
	
	char from_lastname[BLOCK_NAME_MAXSIZE+1];
	strcpy(from_lastname, s);
	if ( strcmp(from_lastname, ".") == 0 ) return 2;
	if ( strcmp(from_lastname, "..") == 0 ) return 2;
	unsigned int from_blockindex = blockindex;
	
	// ===========================
	// check to path
	len = (int)strlen(to_filename);
	if ( to_filename[len-1] == '/' ) return 3; // 尾部有'/'，说明to_filename是目录
	if ( to_filename[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
	} else if ( to_filename[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) blockindex = ffs->home_pwd_blockindex; // pwd
		else blockindex = ffs->tmp.home_pwd_blockindex;
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) blockindex = ffs->pwd_blockindex; // pwd
		else blockindex = ffs->tmp.pwd_blockindex;
		start = 0;
	}
	
	slen = 0;
	memset(s, 0, BLOCK_NAME_MAXSIZE+2);
	for (i=start; i<len; i++) {
		if ( to_filename[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			if ( i == len - 1 ) break; // 留下最后一个s不判断
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) {
				return 3; // dir not exist
			}
			blockindex = index;
			slen = 0;
			continue;
		}
		s[slen] = to_filename[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 3; // name to long
	}
	if ( slen > 0 ) {
		s[slen] = 0;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 3; // name to long
	}
	
	char to_lastname[BLOCK_NAME_MAXSIZE+1];
	strcpy(to_lastname, s);
	if ( strcmp(to_lastname, ".") == 0 ) return 3;
	if ( strcmp(to_lastname, "..") == 0 ) return 3;
	unsigned int to_blockindex = blockindex;
	
	//printf("from lastname:%s, to lastname:%s\n", from_lastname, to_lastname);

	// ===========================
	unsigned char b4[4], b2[2];
	int k;
	// ===================================
	// check from filename, exist
	unsigned char from_block[BLOCKSIZE];
	unsigned char state, dir_file;
	unsigned int from_stop_blockindex;
	unsigned short from_offset;
	
	// block_head
	if ( ! readblock(ffs, from_blockindex, from_block) ) return 1;
	memcpy(b4, from_block+(12+1+14+4), 4);
	from_stop_blockindex = B4toU32(b4);
	memcpy(b2, from_block+(12+1+14+4+4), 2);
	from_offset = B2toU16(b2);
	
	// 搜索block，检查是否有名称相同的目录或文件
	unsigned int from_file_start_blockindex, from_file_stop_blockindex;
	unsigned short from_file_offset;
	unsigned char flag = 0;
	index = from_blockindex;
	while (1) {
		// blocksize=512,去掉前置的12byte，一共能放下20个子项目(目录或文件)
		k = BLOCK_HEAD;
		for (i=0; i<BLOCK_ITEM_MAXCOUNT; i++) {
			if ( index == from_stop_blockindex && k+1 >= from_offset ) { // 已搜索到最后
				return 4; // from lastname item not exist
			}
			state = from_block[k]; k++;
			memcpy(s, from_block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
			if ( strcmp(s, from_lastname) != 0 ) {
				k += 10; // 4+4+2
				continue;
			}
			dir_file = state & 0x01; // 0-dir,1-file
			if ( dir_file != 1 ) return 2; // from format err
			memcpy(b4, from_block+k, 4); k += 4;
			from_file_start_blockindex = B4toU32(b4);
			memcpy(b4, from_block+k, 4); k += 4;
			from_file_stop_blockindex = B4toU32(b4);
			memcpy(b2, from_block+k, 2);
			from_file_offset = B2toU16(b2);
			// printf("from_file, start, stop index, offset:%d %d %d\n", from_file_start_blockindex, from_file_stop_blockindex, from_file_offset);
			
			flag = 1;
			break;
		}
		if ( flag ) break;
		
		// get nextblockindex;
		memcpy(b4, from_block+4, 4);
		index = B4toU32(b4);
		if ( index == 0 ) return 1; // 到这里说明block有问题
		if ( ! readblock(ffs, index, from_block) ) return 1; // 到这里说明block有问题
	}
	
	// ===========================
	// check to_lastname, not exist
	BlockArray to_ba[2];
	int to_ba_used = 0;
	unsigned char *to_block_head, *to_block_last;
	unsigned int to_block_head_index, to_block_last_index;
	for (i=0; i<2; i++) to_ba[i].active = 0;
	
	unsigned char to_block[BLOCKSIZE];
	unsigned int to_stop_blockindex;
	unsigned short to_offset;
	
	// block_head
	if ( ! readblock(ffs, to_blockindex, to_block) ) return 1;
	memcpy(to_ba[0].block, to_block, BLOCKSIZE);
	to_ba[0].blockindex = to_blockindex;
	to_ba[0].active = 1;
	to_block_head = to_ba[0].block;
	to_block_head_index = to_blockindex;
	to_ba_used = 1;
	
	memcpy(b4, to_block_head+(12+1+14+4), 4);
	to_stop_blockindex = B4toU32(b4);
	memcpy(b2, to_block_head+(12+1+14+4+4), 2);
	to_offset = B2toU16(b2);
	
	// block_last
	if ( to_stop_blockindex == to_block_head_index ) {
		to_block_last = to_block_head;
		to_block_last_index = to_block_head_index;
	} else {
		if ( ! readblock(ffs, to_stop_blockindex, to_ba[1].block) ) return 1;
		to_ba[1].blockindex = to_stop_blockindex;
		to_ba[1].active = 1;
		to_block_last = to_ba[1].block;
		to_block_last_index = to_stop_blockindex;
		to_ba_used++;
	}
	
	// 搜索block，检查是否有名称相同的目录或文件
	flag = 0;
	index = to_block_head_index;
	while (1) {
		// blocksize=512,去掉前置的12byte，一共能放下20个子项目(目录或文件)
		k = BLOCK_HEAD;
		for (i=0; i<BLOCK_ITEM_MAXCOUNT; i++) {
			// printf("index:%d, to_stop_blockindex:%d, k=%d, offset:%d\n", index, to_stop_blockindex, k, offset);
			if ( index == to_stop_blockindex && k+1 >= to_offset ) { // 已搜索到最后
				flag = 1;
				break; // new lastname item not exist
			}
			
			k++;
			memcpy(s, to_block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
			if ( strcmp(s, to_lastname) != 0 ) {
				k += 10; // 4+4+2
				continue;
			}
			return 5; // to_lastname already exist
		}
		if ( flag ) break;
		
		// get nextblockindex;
		memcpy(b4, to_block+4, 4);
		index = B4toU32(b4);
		if ( index == 0 ) return 1; // 到这里说明block有问题
		if ( ! readblock(ffs, index, to_block) ) return 1; // 到这里说明block有问题
	}
	
	// ===========================
	// add item to to_block_last
	if ( ffs->tmp.state == 0 ) tmpstart(ffs, 1);
	
	// == 在to_block中创建一个新的item，将old_item复制过来
	unsigned int blockindex_2;
	unsigned char block_2[BLOCKSIZE];
	unsigned short new_to_offset;
	
	if ( to_offset < BLOCKSIZE ) { // 最后一个block未填满		
		// 向to_block_last写入lastname目录项
		memset(to_block_last + to_offset, 0, 25);
		to_block_last[to_offset] = 1; // file
		memcpy(to_block_last + to_offset + 1, to_lastname, (int)strlen(to_lastname));
		
		new_to_offset = to_offset + 25;
		U16toB2(new_to_offset, b2);
		memcpy(to_block_head + BLOCK_OFFSET, b2, 2);
	} else { // 最后一个block已填满
		// 创建存储lastname的目录延伸块
		blockindex_2 = genblockindex(ffs);
		if ( blockindex_2 == 0 ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}		
		memset(block_2, 0, BLOCKSIZE);
		// prevblockindex
		U32toB4(to_block_last_index, b4);
		memcpy(block_2+8, b4, 4);
		block_2[BLOCK_HEAD] = 1; // file
		// copy lastname
		memcpy(block_2 + BLOCK_HEAD + 1, to_lastname, (int)strlen(to_lastname));
		
		// to_block_last->nextblockindex = blockindex_2;
		U32toB4(blockindex_2, b4);
		memcpy(to_block_last + 4, b4, 4);
		
		new_to_offset = BLOCK_HEAD + 25;
		U16toB2(new_to_offset, b2);
		memcpy(to_block_head + BLOCK_OFFSET, b2, 2);
		U32toB4(blockindex_2, b4);
		memcpy(to_block_head + BLOCK_STOP_BLOCKINDEX, b4, 4);
	}
	
	// ===========================
	// copy from_content to to_content
	unsigned int to_file_start_blockindex = 0, to_file_stop_blockindex = 0;
	unsigned short to_file_offset = 0;
	
	unsigned int from_index, from_next_index;
	
	unsigned int new_blockindex, prev_index;
	unsigned char new_block[BLOCKSIZE];
	
	if ( from_file_start_blockindex > 0 ) {
		to_file_offset = from_file_offset;
		
		from_index = from_file_start_blockindex;
		if ( ! readblock(ffs, from_index, from_block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		// from_next_index = getnextindex;
		memcpy(b4, from_block + 4, 4);
		from_next_index = B4toU32(b4);
		
		new_blockindex = genblockindex(ffs);
		if ( new_blockindex == 0 ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		to_file_start_blockindex = new_blockindex;
		to_file_stop_blockindex = new_blockindex;
		
		prev_index = 0;
		while (1) {
			memcpy(new_block, from_block, BLOCKSIZE);
			U32toB4(prev_index, b4);
			memcpy(new_block + 8, b4, 4);

			// printf("from index:%d\n", from_index);
			//if ( from_next_index == 0 ) {
			if ( from_index == from_file_stop_blockindex ) {
				to_file_stop_blockindex = new_blockindex;
				if ( ! writeblock(ffs, new_blockindex, new_block) ) {
					if ( ffs->tmp.state == 1 ) tmpstop(ffs);
					return 1;
				}
				break;
			}

			prev_index = new_blockindex;
			new_blockindex = genblockindex(ffs);
			U32toB4(new_blockindex, b4);
			memcpy(new_block + 4, b4, 4);
			if ( ! writeblock(ffs, prev_index, new_block) ) {
				if ( ffs->tmp.state == 1 ) tmpstop(ffs);
				return 1;
			}
			
			from_index = from_next_index;			
			if ( ! readblock(ffs, from_index, from_block) ) {
				if ( ffs->tmp.state == 1 ) tmpstop(ffs);
				return 1;
			}
			memcpy(b4, from_block + 4, 4);
			from_next_index = B4toU32(b4);
		}
	}

	// printf("start:%d, stop:%d, offset:%d\n", to_file_start_blockindex, to_file_stop_blockindex, to_file_offset);
	if ( to_offset < BLOCKSIZE ) {
		U32toB4(to_file_start_blockindex, b4);
		memcpy(to_block_last + to_offset + 25 - 10, b4, 4);
		U32toB4(to_file_stop_blockindex, b4);
		memcpy(to_block_last + to_offset + 25 - 6, b4, 4);
		U16toB2(to_file_offset, b2);
		memcpy(to_block_last + to_offset + 25 - 2, b2, 2);
	} else {
		U32toB4(to_file_start_blockindex, b4);
		memcpy(block_2 + BLOCK_HEAD + 25 - 10, b4, 4);
		U32toB4(to_file_stop_blockindex, b4);
		memcpy(block_2 + BLOCK_HEAD + 25 - 6, b4, 4);
		U16toB2(to_file_offset, b2);
		memcpy(block_2 + BLOCK_HEAD + 25 - 2, b2, 2);
		if ( ! writeblock(ffs, blockindex_2, block_2) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
	}
	
	for (i=0; i<to_ba_used; i++) {
		if ( ! to_ba[i].active ) continue;
		if ( ! writeblock(ffs, to_ba[i].blockindex, to_ba[i].block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
	}
	
	// commit
	if ( ffs->tmp.state == 1 ) {
		if ( ! FileFS_commit(ffs) ) {
			return 1;
		}
	}

	return 0;
}
// =============================================

static unsigned char InitPwdtmp(FileFS *ffs, char *s)
{
	int len = (int)strlen(s);
	void *p;
	
	if ( len+1 > ffs->pwd_tmp_size ) {
		p = realloc(ffs->pwd_tmp, len+1);
		if ( p == NULL ) return 0;
		ffs->pwd_tmp = (char*)p;
		ffs->pwd_tmp_size = len + 1;
	}
	
	strcpy(ffs->pwd_tmp, s);
	return 1;
}
static unsigned char AddToPwdtmp(FileFS *ffs, int pathsize, char *s)
{
	// printf("add to pwdtmp:%s\n", s);
	
	// do nothing
	if ( strcmp(s, ".") == 0 ) return 1;

	int i, len;
	len = (int)strlen(ffs->pwd_tmp);
	
	// 向前搜索到前一个/
	if ( strcmp(s, "..") == 0 ) {
		for (i=1; i<len; i++) {
			if ( ffs->pwd_tmp[len-i-1] == '/' ) {
				ffs->pwd_tmp[len-i] = 0;
				return 1;
			}
		}
		return 0;
	} 

	// add s/ to pwd
	void *p;
	if ( ffs->pwd_tmp_size < len + (int)strlen(s) + 2 ) {
		p = realloc(ffs->pwd_tmp, len+pathsize+2);
		if ( p == NULL ) return 0;
		ffs->pwd_tmp = (char*)p;
		ffs->pwd_tmp_size = len+pathsize+2;
	}
		
	strcat(ffs->pwd_tmp, s);
	strcat(ffs->pwd_tmp, "/");
	return 1;
}
unsigned char FileFS_chdir(FileFS *ffs, const char *pathname)
{
	if ( ffs == NULL ) return 0;
	if ( ffs->fp == NULL ) return 0;
	
	int i, start, len = (int)strlen(pathname);
	unsigned int blockindex;
	if ( pathname[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
		if ( ! InitPwdtmp(ffs, "/") ) {
			//printf("1\n");
			return 0;
		}
	} else if ( pathname[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) {
			blockindex = ffs->home_pwd_blockindex; // pwd
			if ( ! InitPwdtmp(ffs, ffs->home_pwd) ) {
				//printf("2\n");
				return 0;
			}
		} else {
			blockindex = ffs->tmp.home_pwd_blockindex;
			if ( ! InitPwdtmp(ffs, ffs->tmp.home_pwd) ) {
				//printf("3\n");
				return 0;
			}
		}
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) {
			blockindex = ffs->pwd_blockindex; // pwd
			if ( ! InitPwdtmp(ffs, ffs->pwd) ) {
				//printf("4\n");
				return 0;
			}
		} else {
			blockindex = ffs->tmp.pwd_blockindex; // pwd
			if ( ! InitPwdtmp(ffs, ffs->tmp.pwd) ) {
				//printf("5\n");
				return 0;
			}
		}
		start = 0;
	}
	
	void *p;
	char s[BLOCK_NAME_MAXSIZE + 2];
	int slen = 0;
	unsigned int index;
	for (i=start; i<len; i++) {
		if ( pathname[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) {
				//printf("5\n");
				return 0;
			}
			blockindex = index;
			slen = 0;
			
			if ( ! AddToPwdtmp(ffs, len, s) ) {
				//printf("6\n");
				return 0;
			}
			continue;
		}
		s[slen] = pathname[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) {
			//printf("7\n");
			return 0;
		}
	}
	if ( slen > 0 ) {
		s[slen] = 0;
		index = findPathBlockindex(ffs, blockindex, s);
		if ( index < 1 ) {
			//printf("8\n");
			return 0;
		}
		blockindex = index;
		
		if ( ! AddToPwdtmp(ffs, len, s) ) {
			//printf("9\n");
			return 0;
		}
	}
	
	if ( ffs->tmp.state == 0 ) {
		len = (int)strlen(ffs->pwd_tmp) + 1;
		if ( len > ffs->pwd_size ) {
			p = realloc(ffs->pwd, len);
			if ( p == NULL ) {
				//printf("10\n");
				return 0;
			}
			ffs->pwd = (char*)p;
			ffs->pwd_size = len;
		}
		strcpy(ffs->pwd, ffs->pwd_tmp);
		
		ffs->pwd_blockindex = blockindex;
	} else {
		len = (int)strlen(ffs->pwd_tmp) + 1;
		if ( len > ffs->tmp.pwd_size ) {
			p = realloc(ffs->tmp.pwd, len);
			if ( p == NULL ) {
				//printf("11\n");
				return 0;
			}
			ffs->tmp.pwd = (char*)p;
			ffs->tmp.pwd_size = len;
		}
		strcpy(ffs->tmp.pwd, ffs->pwd_tmp);
		
		ffs->tmp.pwd_blockindex = blockindex;
	}
	
	return 1;
}

char *FileFS_getcwd(FileFS *ffs)
{
	if ( ffs == NULL ) return "";
	if ( ffs->fp == NULL ) return "";
	
	if ( ffs->tmp.state == 0 ) {
		return ffs->pwd;
	} else {
		return ffs->tmp.pwd;
	}
}

unsigned char FileFS_sethome(FileFS *ffs, const char *pathname)
{
	if ( ffs == NULL ) return 0;
	if ( ffs->fp == NULL ) return 0;
	
	int i, start, len = (int)strlen(pathname);
	unsigned int blockindex;
	if ( pathname[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
		if ( ! InitPwdtmp(ffs, "/") ) return 0;
	} else if ( pathname[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) {
			blockindex = ffs->home_pwd_blockindex; // pwd
			if ( ! InitPwdtmp(ffs, ffs->home_pwd) ) return 0;
		} else {
			blockindex = ffs->tmp.home_pwd_blockindex;
			if ( ! InitPwdtmp(ffs, ffs->tmp.home_pwd) ) return 0;
		}
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) {
			blockindex = ffs->pwd_blockindex; // pwd
			if ( ! InitPwdtmp(ffs, ffs->pwd) ) return 0;
		} else {
			blockindex = ffs->tmp.pwd_blockindex; // pwd
			if ( ! InitPwdtmp(ffs, ffs->tmp.pwd) ) return 0;
		}
		start = 0;
	}
	
	void *p;
	char s[BLOCK_NAME_MAXSIZE + 2];
	int slen = 0;
	unsigned int index;
	for (i=start; i<len; i++) {
		if ( pathname[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) return 0;
			blockindex = index;
			slen = 0;
			
			if ( ! AddToPwdtmp(ffs, len, s) ) return 0;
			continue;
		}
		s[slen] = pathname[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 0;
	}
	if ( slen > 0 ) {
		s[slen] = 0;
		index = findPathBlockindex(ffs, blockindex, s);
		if ( index < 1 ) return 0;
		blockindex = index;
		
		if ( ! AddToPwdtmp(ffs, len, s) ) return 0;
	}
	
	if ( ffs->tmp.state == 0 ) {
		len = (int)strlen(ffs->pwd_tmp) + 1;
		if ( len > ffs->home_pwd_size ) {
			p = realloc(ffs->home_pwd, len);
			if ( p == NULL ) return 0;
			ffs->home_pwd = (char*)p;
			ffs->home_pwd_size = len;
		}
		strcpy(ffs->home_pwd, ffs->pwd_tmp);
		
		ffs->home_pwd_blockindex = blockindex;
	} else {
		len = (int)strlen(ffs->pwd_tmp) + 1;
		if ( len > ffs->tmp.home_pwd_size ) {
			p = realloc(ffs->tmp.home_pwd, len);
			if ( p == NULL ) return 0;
			ffs->tmp.home_pwd = (char*)p;
			ffs->tmp.home_pwd_size = len;
		}
		strcpy(ffs->tmp.home_pwd, ffs->pwd_tmp);
		
		ffs->tmp.home_pwd_blockindex = blockindex;
	}
	
	return 1;
}

char *FileFS_gethome(FileFS *ffs)
{
	if ( ffs == NULL ) return "";
	if ( ffs->fp == NULL ) return "";
	
	if ( ffs->tmp.state == 0 ) {
		return ffs->home_pwd;
	} else {
		return ffs->tmp.home_pwd;
	}
}

// 将当前目录存储为工作目录，此接口只是为了保留当前目录
unsigned char FileFS_setwork(FileFS *ffs)
{
	if ( ffs == NULL ) return 0;
	if ( ffs->fp == NULL ) return 0;
	
	int len;
	void *p;
	if ( ffs->tmp.state == 0 ) {
		len = (int)strlen(ffs->pwd) + 1;
		if ( len > ffs->work_size ) {
			p = realloc(ffs->work, len);
			if ( p == NULL ) return 0;
			ffs->work = (char*)p;
			ffs->work_size = len;
		}
		strcpy(ffs->work, ffs->pwd);
		
		ffs->work_blockindex = ffs->pwd_blockindex;
	} else {
		len = (int)strlen(ffs->tmp.pwd) + 1;
		if ( len > ffs->tmp.work_size ) {
			p = realloc(ffs->tmp.work, len);
			if ( p == NULL ) return 0;
			ffs->tmp.work = (char*)p;
			ffs->tmp.work_size = len;
		}
		strcpy(ffs->tmp.work, ffs->tmp.pwd);
		
		ffs->tmp.work_blockindex = ffs->tmp.pwd_blockindex;
	}
	
	return 1;
}
// 将当前目录转回工作目录
unsigned char FileFS_chwork(FileFS *ffs)
{
	if ( ffs == NULL ) return 0;
	if ( ffs->fp == NULL ) return 0;
	
	int len;
	void *p;
	if ( ffs->tmp.state == 0 ) {
		if ( ffs->work == NULL ) return 0;
		len = (int)strlen(ffs->work) + 1;
		if ( len > ffs->pwd_size ) {
			p = realloc(ffs->pwd, len);
			if ( p == NULL ) {
				//printf("10\n");
				return 0;
			}
			ffs->pwd = (char*)p;
			ffs->pwd_size = len;
		}
		strcpy(ffs->pwd, ffs->work);
		
		ffs->pwd_blockindex = ffs->work_blockindex;
	} else {
		if ( ffs->tmp.work == NULL ) return 0;
		len = (int)strlen(ffs->tmp.work) + 1;
		if ( len > ffs->tmp.pwd_size ) {
			p = realloc(ffs->tmp.pwd, len);
			if ( p == NULL ) {
				//printf("11\n");
				return 0;
			}
			ffs->tmp.pwd = (char*)p;
			ffs->tmp.pwd_size = len;
		}
		strcpy(ffs->tmp.pwd, ffs->tmp.work);
		
		ffs->tmp.pwd_blockindex = ffs->tmp.work_blockindex;
	}
	
	return 1;
}

// ======================
static int do_mkdir(FileFS *ffs, char *lastname, unsigned int start_blockindex, unsigned char *start_block, 
	unsigned int cur_blockindex, unsigned char *cur_block, 
	unsigned int stop_blockindex, unsigned short offset)
{
	if ( ffs->tmp.state == 0 ) tmpstart(ffs, 1);
	
	//printf("start_blockindex:%d, cur_blockindex:%d, stop_blockindex:%d, offset:%d\n",
		//start_blockindex, cur_blockindex, stop_blockindex, offset);
	
	int k;
	unsigned char new_block[BLOCKSIZE], block_2[BLOCKSIZE];
	unsigned int new_blockindex, blockindex_2;
	unsigned char b4[4], b2[2], state;
	char name[BLOCK_NAME_MAXSIZE + 1];
	unsigned short new_offset, ls;
	
	// 最后一个block未填满
	if ( offset < BLOCKSIZE ) {
		// == 创建lastname所指向的block
		new_blockindex = genblockindex(ffs);
		if ( new_blockindex == 0 ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}		
		memset(new_block, 0, BLOCKSIZE);
		k = BLOCK_HEAD;
		// .
		// state
		state = 0;
		memcpy(new_block+k, &state, 1); 
		k += 1;
		// name
		memset(name, 0, BLOCK_NAME_MAXSIZE+1);
		name[0] = '.';
		memcpy(new_block+k, name, 1); 
		k += BLOCK_NAME_MAXSIZE;
		// start_blockindex, 1
		U32toB4(new_blockindex, b4);
		memcpy(new_block+k, b4, 4); 
		k += 4;
		// stop_blockindex, 0
		memcpy(new_block+k, b4, 4); 
		k += 4;
		// offset, 0(short)
		ls = 4+4+4 + 25 + 25; // tmpindex+nextblockindex+prevblockindex + . + ..
		U16toB2(ls, b2);
		memcpy(new_block+k, b2, 2);
		k += 2;
		
		// ..
		// state
		state = 0;
		memcpy(new_block+k, &state, 1); 
		k += 1;
		// name
		name[1] = '.'; // ..
		memcpy(new_block+k, name, 2); 
		k += BLOCK_NAME_MAXSIZE;
		// start_blockindex, 0
		U32toB4(start_blockindex, b4);
		memcpy(new_block+k, b4, 4); 
		k += 4;
		// stop_blockindex, 0
		k += 4;
		// offset, 0
		k += 2;
		if ( ! writeblock(ffs, new_blockindex, new_block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		
		// == 向cur_block写入lastname目录项
		k = offset;
		
		// state
		cur_block[k] = 0; 
		k++; 
		
		memset(cur_block+k, 0, BLOCK_NAME_MAXSIZE);
		memcpy(cur_block+k, lastname, (int)strlen(lastname));
		k += BLOCK_NAME_MAXSIZE;
		
		U32toB4(new_blockindex, b4);
		memcpy(cur_block+k, b4, 4);
		k += 4; // start_blockindex
		k += 4; // stop_blockindex
		k += 2; // offset
		new_offset = k;
		U16toB2(new_offset, b2);
		//printf("new_offset:%d\n", new_offset);
		
		if ( cur_blockindex == start_blockindex ) { // 当前目录只有1个block
			memcpy(cur_block + BLOCK_OFFSET, b2, 2);
			if ( ! writeblock(ffs, cur_blockindex, cur_block) ) {
				if ( ffs->tmp.state == 1 ) tmpstop(ffs);
				return 1;
			}
			if ( ffs->tmp.state == 1 ) {
				if ( ! FileFS_commit(ffs) ) return 1;
			}
		} else { // 当前目录有多个block
			if ( ! writeblock(ffs, cur_blockindex, cur_block) ) {
				if ( ffs->tmp.state == 1 ) tmpstop(ffs);
				return 1;
			}
			memcpy(start_block + BLOCK_OFFSET, b2, 2);
			if ( ! writeblock(ffs, start_blockindex, start_block) ) {
				if ( ffs->tmp.state == 1 ) tmpstop(ffs);
				return 1;
			}
			
			if ( ffs->tmp.state == 1 ) {
				if ( ! FileFS_commit(ffs) ) {
					return 1;
				}
			}
		}
		
		return 0;
	}
	
	// 最后一个block已填满
	// ======================================
	new_blockindex = genblockindex(ffs); // 提前生成lastname指向的目录块
	if ( new_blockindex == 0 ) {
		if ( ffs->tmp.state == 1 ) tmpstop(ffs);
		return 1;
	}	
	// 创建存储lastname的目录延伸块
	// gen block_2 for lastname;
	// block_2->prevblockindex = cur_blockindex;
	// write;
	blockindex_2 = genblockindex(ffs);
	if ( blockindex_2 == 0 ) {
		if ( ffs->tmp.state == 1 ) tmpstop(ffs);
		return 1;
	}		
	memset(block_2, 0, BLOCKSIZE);
	k = 8;
	// prevblockindex
	U32toB4(cur_blockindex, b4);
	memcpy(block_2+k, b4, 4);
	k += 4;
	
	// state
	block_2[k] = 0; 
	k++; 
	
	memset(block_2+k, 0, BLOCK_NAME_MAXSIZE);
	memcpy(block_2+k, lastname, (int)strlen(lastname));
	k += BLOCK_NAME_MAXSIZE;
	
	U32toB4(new_blockindex, b4);
	memcpy(block_2+k, b4, 4);
	k += 4; // start_blockindex
	k += 4; // stop_blockindex
	k += 2; // offset
	new_offset = k;
		
	if ( ! writeblock(ffs, blockindex_2, block_2) ) {
		if ( ffs->tmp.state == 1 ) tmpstop(ffs);
		return 1;
	}
	
	// gen new block for ./..;
	// write;
	memset(new_block, 0, BLOCKSIZE);
	k = BLOCK_HEAD;
	// .
	// state
	state = 0;
	memcpy(new_block+k, &state, 1); 
	k += 1;
	// name
	memset(name, 0, BLOCK_NAME_MAXSIZE+1);
	name[0] = '.';
	memcpy(new_block+k, name, 1); 
	k += BLOCK_NAME_MAXSIZE;
	// start_blockindex, 1
	U32toB4(new_blockindex, b4);
	memcpy(new_block+k, b4, 4); 
	k += 4;
	// stop_blockindex, 0
	memcpy(new_block+k, b4, 4); 
	k += 4;
	// offset, 0(short)
	ls = 4+4+4 + 25 + 25; // tmpindex+nextblockindex+prevblockindex + . + ..
	U16toB2(ls, b2);
	memcpy(new_block+k, b2, 2);
	k += 2;
	
	// ..
	// state
	state = 0;
	memcpy(new_block+k, &state, 1); 
	k += 1;
	// name
	name[1] = '.'; // ..
	memcpy(new_block+k, name, 2); 
	k += BLOCK_NAME_MAXSIZE;
	// start_blockindex, 0
	U32toB4(start_blockindex, b4);
	memcpy(new_block+k, b4, 4); 
	k += 4;
	// stop_blockindex, 0
	k += 4;
	// offset, 0
	k += 2;
	if ( ! writeblock(ffs, new_blockindex, new_block) ) {
		if ( ffs->tmp.state == 1 ) tmpstop(ffs);
		return 1;
	}
	
	U16toB2(new_offset, b2);
	// cur_block->nextblockindex = blockindex_2;
	U32toB4(blockindex_2, b4);
	memcpy(cur_block + 4, b4, 4);
	
	if ( cur_blockindex == start_blockindex ) { // 当前目录只有1个block
		// modify stop_blockindex;
		// modify offset;
		// write cur_block;
		memcpy(cur_block + BLOCK_STOP_BLOCKINDEX, b4, 4);
		memcpy(cur_block + BLOCK_OFFSET, b2, 2);
		if ( ! writeblock(ffs, cur_blockindex, cur_block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		if ( ffs->tmp.state == 1 ) {
			if ( ! FileFS_commit(ffs) ) {
				return 1;
			}
		}
	} else { // 当前目录有多个block
		// write cur_block;
		// modify stop_blockindex by start_block;
		// modify offset by start_block;
		// write start_block;
		if ( ! writeblock(ffs, cur_blockindex, cur_block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		memcpy(start_block + BLOCK_STOP_BLOCKINDEX, b4, 4);
		memcpy(start_block + BLOCK_OFFSET, b2, 2);
		if ( ! writeblock(ffs, start_blockindex, start_block) ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		
		if ( ffs->tmp.state == 1 ) {
			if ( ! FileFS_commit(ffs) ) {
				return 1;
			}
		}
	}
	
	return 0;
}

// return: 0-ok,1-gen err,2-name>limit(14byte),3-dirtroy existed,4-exist same name file
int FileFS_mkdir(FileFS *ffs, const char *pathname)
{
	if ( pathname == NULL ) return 1;
	if ( ffs == NULL ) return 1;
	if ( ffs->fp == NULL ) return 1;
	
	int i, start, len = (int)strlen(pathname);
	unsigned int blockindex;
	if ( pathname[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
	} else if ( pathname[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) blockindex = ffs->home_pwd_blockindex; // pwd
		else blockindex = ffs->tmp.home_pwd_blockindex;
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) blockindex = ffs->pwd_blockindex; // pwd
		else blockindex = ffs->tmp.pwd_blockindex;
		start = 0;
	}
	
	char s[BLOCK_NAME_MAXSIZE+2];
	int slen = 0;
	unsigned int index;
	memset(s, 0, BLOCK_NAME_MAXSIZE+2);
	for (i=start; i<len; i++) {
		if ( pathname[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) {
				if ( i == len-1 ) break;
				return 1;
			}
			blockindex = index;
			slen = 0;
			continue;
		}
		s[slen] = pathname[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 2;
	}
	if ( slen == 0 ) {
		// pathname作为目录形式全部搜索完毕且都存在,意味着无需创建
		return 3;
	}
	
	// slen > 0
	s[slen] = 0; // 此时的s是形似xx/yy/zz/abc最后的abc，可能是文件，也可能是目录
	if ( (int)strlen(s) > BLOCK_NAME_MAXSIZE ) return 2;
	char lastname[BLOCK_NAME_MAXSIZE+1];
	strcpy(lastname, s);
	
	// ===============================
	unsigned char start_block[BLOCKSIZE], block[BLOCKSIZE];
	unsigned char state, dir_file;
	unsigned char b4[4], b2[2];
	unsigned int start_blockindex, stop_blockindex;
	unsigned short offset;
	int k;
	
	// printf("blockindex:%d\n", blockindex);
	if ( ! readblock(ffs, blockindex, block) ) return 1;
	memcpy(start_block, block, BLOCKSIZE);
	start_blockindex = blockindex;
	
	memcpy(b4, block+(12+1+14+4), 4);
	stop_blockindex = B4toU32(b4);
	memcpy(b2, block+(12+1+14+4+4), 2);
	offset = B2toU16(b2);
	
	// 搜索block，检查是否有名称相同的目录或文件
	unsigned char flag = 0;
	index = start_blockindex;
	while (1) {
		// blocksize=512,去掉前置的12byte，一共能放下20个子项目(目录或文件)
		k = BLOCK_HEAD;
		for (i=0; i<BLOCK_ITEM_MAXCOUNT; i++) {
			// printf("index:%d, stop_blockindex:%d, k=%d, offset:%d\n", index, stop_blockindex, k, offset);
			if ( index == stop_blockindex && k+1 >= offset ) { // 已搜索到最后
				flag = 1;
				break;
			}
			
			state = block[k]; k++;
			memcpy(s, block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
			if ( strcmp(s, lastname) != 0 ) {
				k += 10; // 4+4+2
				continue;
			}
			dir_file = state & 0x01;
			if ( dir_file == 0 ) return 3; // same dir exist;
			return 4; // same file exist
		}
		if ( flag ) break;
		
		// get nextblockindex;
		memcpy(b4, block+4, 4);
		index = B4toU32(b4);
		if ( index == 0 ) return 1; // 到这里说明block有问题
		if ( ! readblock(ffs, index, block) ) return 1; // 到这里说明block有问题
	}
	
	// 正式开始生成目录项
	return do_mkdir(ffs, lastname, start_blockindex, start_block, index, block, stop_blockindex, offset);
}

// ========================================
// return: 0-ok,1-gen err,2-sub dir not empty,3-dirtroy not existed,4-name>limit(14byte)
int FileFS_rmdir(FileFS *ffs, const char *pathname)
{
	if ( pathname == NULL ) return 1;
	if ( ffs == NULL ) return 1;
	if ( ffs->fp == NULL ) return 1;
	
	int i, start, len = (int)strlen(pathname);
	unsigned int blockindex;
	if ( pathname[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
	} else if ( pathname[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) blockindex = ffs->home_pwd_blockindex; // pwd
		else blockindex = ffs->tmp.home_pwd_blockindex;
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) blockindex = ffs->pwd_blockindex; // pwd
		else blockindex = ffs->tmp.pwd_blockindex;
		start = 0;
	}
	
	char s[BLOCK_NAME_MAXSIZE+2];
	int slen = 0;
	unsigned int index;
	memset(s, 0, BLOCK_NAME_MAXSIZE+2);
	for (i=start; i<len; i++) {
		if ( pathname[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			if ( i == len - 1 ) break; // 留下最后一个s不判断
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) {
				return 3; // dir not exist
			}
			blockindex = index;
			slen = 0;
			continue;
		}
		s[slen] = pathname[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 4; // name to long
	}
	if ( slen > 0 ) {
		s[slen] = 0;
		if ( slen > BLOCK_NAME_MAXSIZE ) return 4; // name to long
	}
	
	char lastname[BLOCK_NAME_MAXSIZE+1];
	strcpy(lastname, s);
	if ( strcmp(lastname, ".") == 0 ) return 1;
	if ( strcmp(lastname, "..") == 0 ) return 1;
	
	// ===============================
	BlockArray ba[4];
	int ba_used = 0;
	unsigned char *block_head, *block_last, *block_item, *block_prev;
	unsigned int block_item_index, block_last_index, block_prev_index, block_head_index;
	for (i=0; i<4; i++) ba[i].active = 0;
	
	unsigned char block[BLOCKSIZE];
	unsigned char state, dir_file;
	unsigned char b4[4], b2[2];
	unsigned int stop_blockindex;
	unsigned short offset;
	int k;
	
	// block_head
	if ( ! readblock(ffs, blockindex, block) ) return 1;
	memcpy(ba[0].block, block, BLOCKSIZE);
	ba[0].blockindex = blockindex;
	ba[0].active = 1;
	block_head = ba[0].block;
	block_head_index = blockindex;
	ba_used = 1;
	
	memcpy(b4, block_head+(12+1+14+4), 4);
	stop_blockindex = B4toU32(b4);
	memcpy(b2, block_head+(12+1+14+4+4), 2);
	offset = B2toU16(b2);
	
	// block_last
	if ( stop_blockindex == block_head_index ) {
		block_last = block_head;
		block_last_index = block_head_index;
	} else {
		if ( ! readblock(ffs, stop_blockindex, ba[1].block) ) return 1;
		ba[1].blockindex = stop_blockindex;
		ba[1].active = 1;
		block_last = ba[1].block;
		block_last_index = stop_blockindex;
		ba_used++;
	}
	
	// 搜索block，检查是否有名称相同的目录或文件
	unsigned int subdirblockindex;
	unsigned char subdirblock[BLOCKSIZE];
	unsigned int subdir_start_blockindex, subdir_stop_blockindex;
	unsigned short subdir_offset;
	
	unsigned short item_offset = 0;
	
	unsigned char flag = 0, u;
	index = block_head_index;
	while (1) {
		// blocksize=512,去掉前置的12byte，一共能放下20个子项目(目录或文件)
		k = BLOCK_HEAD;
		for (i=0; i<BLOCK_ITEM_MAXCOUNT; i++) {
			// printf("index:%d, stop_blockindex:%d, k=%d, offset:%d\n", index, stop_blockindex, k, offset);
			if ( index == stop_blockindex && k+1 >= offset ) { // 已搜索到最后
				return 3; // dir item not exist
			}
			
			state = block[k]; k++;
			memcpy(s, block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
			if ( strcmp(s, lastname) != 0 ) {
				k += 10; // 4+4+2
				continue;
			}
			//printf("state:%d\n", state);
			dir_file = state & 0x01;
			if ( dir_file == 1 ) { // file
				return 3; // same filename exist;
			}
			
			// 检测子目录是否为空
			memcpy(b4, block+k, 4); subdirblockindex = B4toU32(b4);
			if ( ! readblock(ffs, subdirblockindex, subdirblock) ) return 1;
			// get sub dir blockindex and offset
			memcpy(b4, subdirblock+BLOCK_START_BLOCKINDEX, 4); subdir_start_blockindex = B4toU32(b4);
			memcpy(b4, subdirblock+BLOCK_STOP_BLOCKINDEX, 4); subdir_stop_blockindex = B4toU32(b4);
			memcpy(b2, subdirblock+BLOCK_OFFSET, 2); subdir_offset = B2toU16(b2);
			if ( subdir_stop_blockindex != subdir_start_blockindex ) return 2; // sub dir not empty
			if ( subdir_offset > 62 ) return 2; // sub dir not empty
			
			item_offset = k + 10; // block item的尾部位置
			
			// block_item
			u = 0;
			for (i=0; i<ba_used; i++) {
				if ( ! ba[i].active ) continue;
				if ( ba[i].blockindex == index ) {
					block_item = ba[i].block;
					block_item_index = index;
					u = 1;
					break;
				}
			}
			if ( !u ) {
				memcpy(ba[ba_used].block, block, BLOCKSIZE);
				ba[ba_used].blockindex = index;
				ba[ba_used].active = 1;
				block_item = ba[ba_used].block;
				block_item_index = index;
				ba_used++;
			}
			
			flag = 1;
			break;
		}
		if ( flag ) break;
		
		// get nextblockindex;
		memcpy(b4, block+4, 4);
		index = B4toU32(b4);
		if ( index == 0 ) return 1; // 到这里说明block有问题
		if ( ! readblock(ffs, index, block) ) return 1; // 到这里说明block有问题
	}
	
	// printf("head:%d, item:%d, last:%d, item_offset:%d\n", block_head_index, block_item_index, block_last_index, item_offset);
	// =======================
	// 正式开始删除目录项
	if ( ffs->tmp.state == 0 ) tmpstart(ffs, 1);
	
	// removeblock 子目录
	removeblock(ffs, subdirblockindex);
	
	/*
	if ( item is not offset ) {
		get offset->item;
		copy item;
	}
	1.block_item
	*/
	if ( block_item_index != stop_blockindex || item_offset != offset ) {
		memcpy(block_item + item_offset - 25, block_last + offset - 25, 25);
	}

	/*
	offset -= 25;
	2.block_last
	*/
	offset -= 25;
	U16toB2(offset, b2);
	memcpy(block_head + BLOCK_OFFSET, b2, 2);
	
	/*
	if ( offset < 25 ) { // 整个block都已清空
		get prevblockindex;
		remove block_last;
		prevblock->next_blockindex = 0;
		stop_blockindex = prev_block_index;
		offset = BLOCKSIZE;
		3.block_prev
	}
	4.block_head
	*/
	if ( offset < 25 ) {
		memcpy(b4, block_last+8, 4);
		block_prev_index = B4toU32(b4);
		// printf("block prev index:%d\n", block_prev_index);
		
		removeblock(ffs, block_last_index);
		k = -1;
		for (i=0; i<ba_used; i++) {
			if ( ! ba[i].active ) continue;
			if ( ba[i].blockindex == block_last_index ) {
				ba[i].active = 0;
				k = i;
				break;
			}
		}
		if ( k < 0 ) {
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
		
		// block_prev
		u = 0;
		for (i=0; i<ba_used; i++) {
			if ( ! ba[i].active ) continue;
			if ( ba[i].blockindex == block_prev_index ) {
				block_prev = ba[i].block;
				u = 1;
				break;
			}
		}
		if ( !u ) {
			if ( ! readblock(ffs, block_prev_index, block) ) {
				if ( ffs->tmp.state == 1 ) tmpstop(ffs);
				return 1; // 到这里说明block有问题
			}
			
			memcpy(ba[k].block, block, BLOCKSIZE);
			ba[k].blockindex = block_prev_index;
			ba[k].active = 1;
			block_prev = ba[k].block;
		}
		
		// prevblock->next_blockindex = 0;
		memset(block_prev + 4, 0, 4);
		
		U32toB4(block_prev_index, b4);
		memcpy(block_head + BLOCK_STOP_BLOCKINDEX, b4, 4);
		offset = BLOCKSIZE;
		U16toB2(offset, b2);
		memcpy(block_head + BLOCK_OFFSET, b2, 2);
	}
	
	for (i=0; i<ba_used; i++) {
		// printf("%d: write blockindex:%d\n", i, ba[i].blockindex);
		if ( ! ba[i].active ) continue;
		/*
		for (k=0; k<BLOCKSIZE; k++) {
			printf("%x ", ba[i].block[k]);
		}
		printf("\n");
		*/
		if ( ! writeblock(ffs, ba[i].blockindex, ba[i].block) ) {
			// printf("write block err %d\n", i);
			if ( ffs->tmp.state == 1 ) tmpstop(ffs);
			return 1;
		}
	}
	
	if ( ffs->tmp.state == 1 ) {
		if ( ! FileFS_commit(ffs) ) {
			return 1;
		}
	}
	
	return 0;
}

// =============================================
FFS_DIR *FileFS_opendir(FileFS *ffs, const char *path, char **absolute_path)
{
	if ( ffs == NULL ) return NULL;
	if ( ffs->fp == NULL ) return NULL;
	
	FFS_DIR *dir = (FFS_DIR*)malloc(sizeof(FFS_DIR));
	if ( dir == NULL ) return NULL;
	memset(dir, 0, sizeof(FFS_DIR));
	
	int i, start, len = (int)strlen(path);
	unsigned int blockindex;
	if ( path[0] == '/' ) {
		blockindex = 1; // root
		start = 1;
		if ( ! InitPwdtmp(ffs, "/") ) return 0;
	} else if ( path[0] == '~' ) { // home
		if ( ffs->tmp.state == 0 ) {
			blockindex = ffs->home_pwd_blockindex; // pwd
			if ( ! InitPwdtmp(ffs, ffs->home_pwd) ) return 0;
		} else {
			blockindex = ffs->tmp.home_pwd_blockindex;
			if ( ! InitPwdtmp(ffs, ffs->tmp.home_pwd) ) return 0;
		}
		start = 1;
	} else {
		if ( ffs->tmp.state == 0 ) {
			blockindex = ffs->pwd_blockindex; // pwd
			if ( ! InitPwdtmp(ffs, ffs->pwd) ) return 0;
		} else {
			blockindex = ffs->tmp.pwd_blockindex; // pwd
			if ( ! InitPwdtmp(ffs, ffs->tmp.pwd) ) return 0;
		}
		start = 0;
	}
	
	// printf("pwd blockindex:%d\n", ffs->pwd_blockindex);
	
	char s[BLOCK_NAME_MAXSIZE + 2];
	int slen = 0;
	unsigned int index;
	for (i=start; i<len; i++) {
		if ( path[i] == '/' ) {
			if ( slen == 0 ) continue;
			s[slen] = 0;
			index = findPathBlockindex(ffs, blockindex, s);
			if ( index < 1 ) {
				free(dir);
				return NULL;
			}
			blockindex = index;
			slen = 0;
			
			if ( ! AddToPwdtmp(ffs, len, s) ) return 0;
			continue;
		}
		s[slen] = path[i];
		slen++;
		if ( slen > BLOCK_NAME_MAXSIZE ) {
			free(dir);
			return NULL;
		}
	}
	if ( slen > 0 ) {
		s[slen] = 0;
		index = findPathBlockindex(ffs, blockindex, s);
		if ( index < 1 ) {
			free(dir);
			return NULL;
		}
		//printf("p:%s\n", s);
		blockindex = index;
		
		if ( ! AddToPwdtmp(ffs, len, s) ) return 0;
	}
	
	if ( ! readblock(ffs, blockindex, dir->block) ) {
		free(dir);
		return NULL;
	}
	
	unsigned char b4[4], b2[2];
	memcpy(b4, dir->block+(12+1+14+4), 4);
	dir->stop_blockindex = B4toU32(b4);
	memcpy(b2, dir->block+(12+1+14+4+4), 2);
	dir->offset = B2toU16(b2);
	
	dir->blockindex = blockindex;
	dir->searchindex = 0;
	
	*absolute_path = ffs->pwd_tmp;
	
	return dir;
}

FFS_dirent *FileFS_readdir(FileFS *ffs, FFS_DIR *dir)
{
	if ( ffs == NULL ) return NULL;
	if ( ffs->fp == NULL ) return NULL;
	if ( dir == NULL ) return NULL;

	unsigned int nextindex;
	unsigned char *block = dir->block;
	unsigned char state, dir_file;
	int k;
	char *s;
	unsigned char b4[4], b2[2];
	unsigned int dirblockindex;
	
	// read block again
	if ( ! readblock(ffs, dir->blockindex, dir->block) ) return NULL;
	memcpy(b4, dir->block+(12+1+14+4), 4);
	dir->stop_blockindex = B4toU32(b4);
	memcpy(b2, dir->block+(12+1+14+4+4), 2);
	dir->offset = B2toU16(b2);
	
	k = BLOCK_HEAD + dir->searchindex * 25;
	if ( dir->blockindex == dir->stop_blockindex && k+1 >= dir->offset ) return NULL; // end;
	while (1) {
		if ( dir->searchindex >= BLOCK_ITEM_MAXCOUNT ) {
			nextindex = B4toU32(block+4); // block前4个byte为next blockindex
			if ( nextindex == 0 ) return NULL; // end
			if ( ! readblock(ffs, nextindex, dir->block) ) return NULL;
			block = dir->block;
			dir->searchindex = 0;
			dir->blockindex = nextindex;
			k = BLOCK_HEAD + dir->searchindex * 25;
			continue;
		}
		
		state = block[k]; k++;
		dir_file = state & 0x01;
		if ( dir_file == 1 ) {
			dir->dirp.d_type = FFS_DT_FILE; // file
		} else {
			dir->dirp.d_type = FFS_DT_DIR; // directory
		}
		
		s = dir->dirp.d_name;
		memset(s, 0, BLOCK_NAME_MAXSIZE+1);
		memcpy(s, block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
		dir->dirp.d_namlen = (int)strlen(s);
		if ( strcmp(s, ".") == 0 ) {
			memcpy(b4, block+k, 4);
			dirblockindex = B4toU32(b4);
			if ( dirblockindex == 1 ) { // root
				dir->dirp.d_type = FFS_DT_ROOT;
			}
		} else if ( strcmp(s, "..") == 0 ) {
			memcpy(b4, block+k, 4);
			dirblockindex = B4toU32(b4);
			if ( dirblockindex == 0 ) { // root
				dir->dirp.d_type = FFS_DT_ROOT;
			}
		}
		
		k += 10;
		dir->searchindex++;
		return &(dir->dirp);
	}
	
	return NULL; // 正常情况下不会执行到这里
}

void FileFS_closedir(FileFS *ffs, FFS_DIR *dir)
{
	if ( dir != NULL ) free(dir);
}

// ====================================
static unsigned int findPathBlockindex(FileFS *ffs, unsigned int blockindex, char *pathname)
{
	unsigned char block[BLOCKSIZE];
	unsigned int index = blockindex;
	unsigned char b4[4], b2[2];
	unsigned char state, dir_file;
	int i, k;
	char s[BLOCK_NAME_MAXSIZE+1];
	memset(s, 0, BLOCK_NAME_MAXSIZE+1);
	unsigned int stop_blockindex;
	unsigned short offset;
	
	if ( ! readblock(ffs, index, block) ) return 0;
	memcpy(b4, block+(BLOCK_STOP_BLOCKINDEX), 4);
	stop_blockindex = B4toU32(b4);
	memcpy(b2, block+(BLOCK_OFFSET), 2);
	offset = B2toU16(b2);
	
	while (1) {
		// blocksize=512,去掉前置的12byte，一共能放下20个子项目(目录或文件)
		k = BLOCK_HEAD;
		for (i=0; i<BLOCK_ITEM_MAXCOUNT; i++) {
			if ( index == stop_blockindex && k+1 >= offset ) return 0; // 已搜索到最后
			state = block[k]; k++;
			dir_file = state & 0x01;
			if ( dir_file == 1 ) { // is file
				k += 24;
				continue;
			}
			// path
			memcpy(s, block+k, BLOCK_NAME_MAXSIZE); k+=BLOCK_NAME_MAXSIZE;
			if ( strcmp(s, pathname) != 0 ) {
				k += 10; // 4+4+2
				continue;
			}
			memcpy(b4, block+k, 4);
			return B4toU32(b4); // find
		}
		
		// get nextblockindex;
		memcpy(b4, block+4, 4);
		index = B4toU32(b4);
		if ( index == 0 ) return 0;		
		if ( ! readblock(ffs, index, block) ) return 0;
	}
	
	return 0;
}

// =================================
unsigned char FileFS_begin(FileFS *ffs)
{
	//printf("begin\n");
	if ( ffs == NULL ) return 0;
	if ( ffs->fp == NULL ) return 0;
	
	return tmpstart(ffs, 2);
}

void FileFS_rollback(FileFS *ffs)
{
	//printf("rollback\n");
	if ( ffs == NULL ) return;
	if ( ffs->fp == NULL ) return;
	
	// clear fpj
	// ffs_remove(ffs->fnj);
	unsigned char state = 0;
	fseek(ffs->fpj, 4, SEEK_SET);
	fwrite(&state, 1, 1, ffs->fpj);
	ffs_fflush(ffs->fpj);
	
	tmpstop(ffs);
}

unsigned char FileFS_commit(FileFS *ffs)
{
	// printf("commit\n");
	if ( ffs == NULL ) return 0;
	if ( ffs->fp == NULL ) return 0;
	
	{
		// write fnj;
		FILE *fp = ffs->fpj;
		unsigned char signal;
		unsigned int blocksize = 0;
		unsigned char b4[4];
		unsigned char block[BLOCKSIZE+4];
		int k;
		unsigned int blockindex;
		unsigned long long pos;
		unsigned int n;
		
		if ( fp == NULL ) {
			fp = fopen(ffs->fnj, "w+b");
			if ( fp == NULL ) {
				tmpstop(ffs);
				return 0;
			}
			ffs->fpj = fp;
		} else {
			ffs_rewind(fp);
		}
		
		// blocksize
		memset(b4, 0, 4);
		if ( 4 != ffs_fwrite(b4, 1, 4, fp) ) {
			ffs_fclose(fp);
			tmpstop(ffs);
			return 0;
		}
		blocksize = 0;
		
		// write byte[0] = 0;
		signal = 0;
		if ( 1 != ffs_fwrite(&signal, 1, 1, fp) ) {
			ffs_fclose(fp);
			tmpstop(ffs);
			return 0;
		}
		
		// block 0
		if ( ffs->tmp.total_blocksize != ffs->tmp.new_total_blocksize ||
			ffs->tmp.unused_blockhead != ffs->tmp.new_unused_blockhead ) {
			// block index = 0
			memset(b4, 0, 4);
			fwrite(b4, 1, 4, fp);
			
			// block
			memset(block, 0, BLOCKSIZE);
			k = 0;
			// magic number	
			memcpy(block+k, magic_number, 4); k += 4;
			// block size;
			U32toB4(ffs->tmp.new_total_blocksize, b4);
			memcpy(block+k, b4, 4); k += 4;
			// unused block head
			U32toB4(ffs->tmp.new_unused_blockhead, b4);
			memcpy(block+k, b4, 4); k += 4;
			// other,皆为0
			fwrite(block, 1, BLOCKSIZE, fp);
		}
		blocksize++;
		
		// copy fp_cp to fnj
		ffs_rewind(ffs->tmp.fp_cp);
		n = 0;
		while (1) {
			if ( n >= ffs->tmp.cp_size ) break;
			n++;
			
			if ( BLOCKSIZE+4 != ffs_fread(block, 1, BLOCKSIZE+4, ffs->tmp.fp_cp) ) break;
			if ( BLOCKSIZE + 4 != ffs_fwrite(block, 1, BLOCKSIZE+4, fp) ) {
				ffs_fclose(fp);
				tmpstop(ffs);
				return 0;
			}
			blocksize++;
		}
		
		// copy fp_add to fnj
		ffs_rewind(ffs->tmp.fp_add);
		n = 0;
		while (1) {
			if ( n >= ffs->tmp.add_size ) break;
			n++;
			
			if ( BLOCKSIZE+4 != ffs_fread(block, 1, BLOCKSIZE+4, ffs->tmp.fp_add) ) break;
			if ( BLOCKSIZE + 4 != ffs_fwrite(block, 1, BLOCKSIZE+4, fp) ) {
				ffs_fclose(fp);
				tmpstop(ffs);
				return 0;
			}
			blocksize++;
		}
		
		ffs_rewind(fp);
		// write blocksize
		U32toB4(blocksize, b4);
		if ( 4 != ffs_fwrite(b4, 1, 4, fp) ) {
			ffs_fclose(fp);
			tmpstop(ffs);
			return 0;
		}
		
		// write byte[0] = 0xff;
		signal = 0xff;
		if ( 1 != ffs_fwrite(&signal, 1, 1, fp) ) {
			ffs_fclose(fp);
			tmpstop(ffs);
			return 0;
		}
		// fsync(fnj);
		ffs_fflush(fp);
		// ffs_fclose(fp);
		
		// ===========================
		// re-read fpj
		pos = 4;
		ffs_fsetpos(fp, pos);
		// fseek(ffs->fpj, 4, SEEK_SET);
		if ( 1 != ffs_fread(&signal, 1, 1, fp) ) {
			ffs_fclose(fp);
			tmpstop(ffs);
			return 0;
		}
		n = 0;
		while (1) {
			if ( n >= blocksize ) break;
			n++;
			
			// read fnj;
			if ( BLOCKSIZE+4 != ffs_fread(block, 1, BLOCKSIZE+4, fp) ) break;
			// gen blockindex;
			blockindex = B4toU32(block);
			// ffs_fwrite(block, 1, blockindex*BLOCKSIZE, ffs->fp);
			pos = blockindex;
			pos *= BLOCKSIZE;
			ffs_fsetpos(ffs->fp, pos);
			if ( BLOCKSIZE != ffs_fwrite(block+4, 1, BLOCKSIZE, ffs->fp) ) {
				ffs_fclose(fp);
				tmpstop(ffs);
				return 0;
			}
		}
		// ffs_fclose(fp);

		// printf("sync 3\n");
		//fsync(ffs->fp);
		ffs_fflush(ffs->fp);
		// printf("sync 4\n");

		// clear fpj
		// ffs_remove(ffs->fnj);
		signal = 0;
		pos = 4;
		ffs_fsetpos(fp, pos);
		fwrite(&signal, 1, 1, ffs->fpj);
		ffs_fflush(ffs->fpj);
	}
	
	int len;
	void *p;
	len = (int)strlen(ffs->tmp.pwd) + 1;
	if ( len > ffs->pwd_size ) {
		p = realloc(ffs->pwd, len);
		if ( p == NULL ) {
			sprintf(ffs->pwd, "/");
			tmpstop(ffs);
			return 1;
		}
		ffs->pwd = (char*)p;
		ffs->pwd_size = len;
	}
	strcpy(ffs->pwd, ffs->tmp.pwd);
	ffs->pwd_blockindex = ffs->tmp.pwd_blockindex;
	
	len = (int)strlen(ffs->tmp.home_pwd) + 1;
	if ( len > ffs->home_pwd_size ) {
		p = realloc(ffs->home_pwd, len);
		if ( p == NULL ) {
			sprintf(ffs->home_pwd, "/");
			tmpstop(ffs);
			return 1;
		}
		ffs->home_pwd = (char*)p;
		ffs->home_pwd_size = len;
	}
	strcpy(ffs->home_pwd, ffs->tmp.home_pwd);
	ffs->home_pwd_blockindex = ffs->tmp.home_pwd_blockindex;
	
	// printf("------commit end-------------\n");
	
	tmpstop(ffs);
	return 1;
}

// ============================================
static unsigned char tmpstart(FileFS *ffs, unsigned char state)
{
	if ( state == 0 ) return 0;
	
	//printf("tmpstart:%d\n", state);
	
	if ( ffs->tmp.state != 0 ) tmpstop(ffs);
	
	// read total_blocksize, unused_blockhead
	unsigned char block[12];
	ffs_rewind(ffs->fp);
	if ( 12 != ffs_fread(block, 1, 12, ffs->fp) ) return 0;
	ffs->tmp.total_blocksize = B4toU32(block+4);
	ffs->tmp.unused_blockhead = B4toU32(block+8);
	ffs->tmp.new_total_blocksize = ffs->tmp.total_blocksize;
	ffs->tmp.new_unused_blockhead = ffs->tmp.unused_blockhead;
	// printf("6.set new_unused_blockhead:%d\n", ffs->tmp.new_unused_blockhead);
	
	if ( ffs->tmp.fp_cp == NULL ) {
		ffs->tmp.fp_cp = ffs_tmpfile();
		if ( ffs->tmp.fp_cp == NULL ) return 0;
	} else {
		fseek(ffs->tmp.fp_cp, 0, SEEK_SET);
	}
	if ( ffs->tmp.fp_add == NULL ) {
		ffs->tmp.fp_add = ffs_tmpfile();
		if ( ffs->tmp.fp_add == NULL ) return 0;
	} else {
		fseek(ffs->tmp.fp_add, 0, SEEK_SET);
	}
	ffs->tmp.cp_size = ffs->tmp.add_size = 0;
	
	void *p;
	int len;
	len = (int)strlen(ffs->pwd) + 1;
	if ( len > ffs->tmp.pwd_size ) {
		p = realloc(ffs->tmp.pwd, len);
		if ( p == NULL ) return 0;
		ffs->tmp.pwd = (char*)p;
		ffs->tmp.pwd_size = len;
	}
	strcpy(ffs->tmp.pwd, ffs->pwd);
	ffs->tmp.pwd_blockindex = ffs->pwd_blockindex;
	
	len = (int)strlen(ffs->home_pwd) + 1;
	if ( len > ffs->tmp.home_pwd_size ) {
		p = realloc(ffs->tmp.home_pwd, len);
		if ( p == NULL ) return 0;
		ffs->tmp.home_pwd = (char*)p;
		ffs->tmp.home_pwd_size = len;
	}
	strcpy(ffs->tmp.home_pwd, ffs->home_pwd);
	ffs->tmp.home_pwd_blockindex = ffs->home_pwd_blockindex;
	
	ffs->tmp.state = state;

	return 1;
}

static void tmpstop(FileFS *ffs)
{
	//printf("tmpstop\n");
	/*
	if ( ffs->tmp.fp_cp != NULL ) {
		ffs_fclose(ffs->tmp.fp_cp);
		ffs->tmp.fp_cp = NULL;
	}
	if ( ffs->tmp.fp_add != NULL ) {
		ffs_fclose(ffs->tmp.fp_add);
		ffs->tmp.fp_add = NULL;
	}
	
	ffs->tmp.cp_size = 0;
	*/
	
	ffs->tmp.state = 0;
}
// ============================================
/*
只返回blockindex，block自己创建，因为原本的block内容已无意义，无需读取具体内容
生成的blockindex必须要写入，否则blockindex指向的block会变成被遗弃的块
return:0-生成失败,other-返回可用的blockindex
*/
static unsigned int genblockindex(FileFS *ffs)
{
	unsigned int blockindex;
	unsigned char block[BLOCKSIZE];
	
	// 从unused_block里取出一个空闲block
	if ( ffs->tmp.new_unused_blockhead > 0 ) {
		blockindex = ffs->tmp.new_unused_blockhead;
		// printf("gen blockindex:%d\n", blockindex);
		if ( ! readblock(ffs, blockindex, block) ) {
			// printf("gen err\n");
			return 0;
		}
		ffs->tmp.new_unused_blockhead = B4toU32(block+4);
		// printf("1.set new_unused_blockhead:%d\n", ffs->tmp.new_unused_blockhead);
		return blockindex;
	}
	
	// unused_block找不到空闲块，则在fp_add里新增一个block，并将new_total_blocksize+1
	blockindex = ffs->tmp.new_total_blocksize;
	unsigned int addindex;
	unsigned long long pos;
	unsigned char b4[4];
	
	addindex = blockindex - ffs->tmp.total_blocksize;
	pos = addindex;
	pos *= (4+BLOCKSIZE);
	ffs_fsetpos(ffs->tmp.fp_add, pos);
	U32toB4(blockindex, b4);
	if ( 4 != ffs_fwrite(b4, 1, 4, ffs->tmp.fp_add) ) return 0;
	if ( BLOCKSIZE != ffs_fwrite(block, 1, BLOCKSIZE, ffs->tmp.fp_add) ) return 0; // block未做任何初始化，因为此时block的数据无任何意义
	ffs->tmp.new_total_blocksize++;
	
	ffs->tmp.add_size++;
	
	return blockindex;
}

/*
读取的block可能来自fp/fp_cp/fp_add中的任一个
*/
static unsigned char readblock(FileFS *ffs, unsigned int blockindex, unsigned char *block)
{
	unsigned long long pos;
	unsigned int addindex;
	unsigned char buf[4], b4[4];
	unsigned int cpindex, orgindex;;
		
	pos = blockindex;
	pos *= BLOCKSIZE;
	ffs_fsetpos(ffs->fp, pos);
	if ( 4 != ffs_fread(buf, 1, 4, ffs->fp) ) { // 超过了fp的文件尺寸
		if ( ffs->tmp.state == 0 ) return 0;
		if ( blockindex < ffs->tmp.total_blocksize ) return 0;
		addindex = blockindex - ffs->tmp.total_blocksize;
		if ( addindex >= ffs->tmp.add_size ) return 0;
		pos = addindex;
		pos *= (BLOCKSIZE+4);
		pos += 4; // 跳过前面的blockindex
		ffs_fsetpos(ffs->tmp.fp_add, pos);
		if ( BLOCKSIZE != ffs_fread(block, 1, BLOCKSIZE, ffs->tmp.fp_add) ) return 0;
		// 因为是增加的block，所以在ffs->fp中没有对应的block，无需处理cpindex
		return 1;
	}
	
	if ( ffs->tmp.state == 0 ) {
		memcpy(block, buf, 4);
		if ( BLOCKSIZE-4 != ffs_fread(block+4, 1, BLOCKSIZE-4, ffs->fp) ) return 0;
		return 1;
	}
	
	// 此时tmp必然存在，且只能在fp_cp中，fp_add已经在前面处理过了，read by fp_cp
	memcpy(b4, buf, 4);
	cpindex = B4toU32(b4);
	if ( cpindex < ffs->tmp.cp_size ) {
		pos = cpindex;
		pos *= (BLOCKSIZE+4);
		ffs_fsetpos(ffs->tmp.fp_cp, pos);
		if ( 4 == ffs_fread(b4, 1, 4, ffs->tmp.fp_cp) ) {
			orgindex = B4toU32(b4);
			// 从fp中读取cpindex，再从指定的cpindex里读取blockindex
			// 比较2个blockindex是否相同，避免fp中写入的cpindex是错误的
			if ( orgindex == blockindex ) {
				// 此时确认fp_cp中存在被复制的block，可以读取fp_cp
				if ( BLOCKSIZE != ffs_fread(block, 1, BLOCKSIZE, ffs->tmp.fp_cp) ) return 0;
				return 1;
			}
		}
	}

	// fp_cp中没有fp的复本，读fp
	memcpy(block, buf, 4);
	if ( BLOCKSIZE-4 != ffs_fread(block+4, 1, BLOCKSIZE-4, ffs->fp) ) return 0;
	return 1;
}

/*
需要写入的block必然存在于fp/fp_cp/fp_add其中之一，肯定不需要创建新的block
1.将block写入fp_cp或fp_add
2.若tmp是第一次写入，则修改fp->block中的cpindex
*/
static unsigned char writeblock(FileFS *ffs, unsigned int blockindex, unsigned char *block)
{
	//if (blockindex == 0) {
		//printf("writeblock: blockindex:%d\n", blockindex);
	//}
	if ( ffs->tmp.state == 0 ) {
		//printf("1\n");
		return 0;
	}
	
	unsigned long long pos;
	unsigned int addindex;
	unsigned char buf[4], b4[4];
	unsigned int cpindex;
	unsigned int orgindex;
	
	pos = blockindex;
	pos *= BLOCKSIZE;
	ffs_fsetpos(ffs->fp, pos);
	if ( 4 != ffs_fread(buf, 1, 4, ffs->fp) ) { // 超过了fp的文件尺寸
		if ( blockindex < ffs->tmp.total_blocksize ) {
			//printf("2\n");
			return 0;
		}
		addindex = blockindex - ffs->tmp.total_blocksize;
		pos = addindex;
		pos *= (BLOCKSIZE+4);
		pos += 4; // 跳过前面的blockindex
		ffs_fsetpos(ffs->tmp.fp_add, pos);
		if ( BLOCKSIZE != ffs_fwrite(block, 1, BLOCKSIZE, ffs->tmp.fp_add) ) {
			//printf("3\n");
			return 0;
		}
		// 因为是增加的block，所以在ffs->fp中没有对应的block，无需处理cpindex
		return 1;
	}
	
	// 此时tmp必然存在，且只能在fp_cp中，read by fp_cp, fp_add已经在前面处理过了
	memcpy(b4, buf, 4);
	cpindex = B4toU32(b4);
	if ( cpindex < ffs->tmp.cp_size ) {
		pos = cpindex;
		pos *= (BLOCKSIZE+4);
		ffs_fsetpos(ffs->tmp.fp_cp, pos);
		if ( 4 == ffs_fread(b4, 1, 4, ffs->tmp.fp_cp) ) {
			orgindex = B4toU32(b4);
			// 从fp中读取cpindex，再从指定的cpindex里读取blockindex
			// 比较2个blockindex是否相同，避免fp中写入的cpindex是错误的
			if ( orgindex == blockindex ) {
				// 在fread和fwrite间切换时:
				// When the “r+”, “w+”, or “a+” access type is specified, both reading and writing are allowed (the file is said to be open for “update”). 
				// However, when you switch between reading and writing, there must be an intervening fflush, fsetpos, fseek, or rewind operation. 
				pos = cpindex;
				pos *= (BLOCKSIZE+4);
				pos += 4;
				ffs_fsetpos(ffs->tmp.fp_cp, pos);
				// printf("write pos:%d, cpindex:%d\n", pos, cpindex);
				if ( BLOCKSIZE != (int)fwrite(block, 1, BLOCKSIZE, ffs->tmp.fp_cp) ) {
					return 0;
				}
				return 1;
			}
		}
	}
	
	cpindex = ffs->tmp.cp_size;
	pos = cpindex;
	pos *= (BLOCKSIZE+4);
	ffs_fsetpos(ffs->tmp.fp_cp, pos);
	U32toB4(blockindex, b4);
	if ( 4 != ffs_fwrite(b4, 1, 4, ffs->tmp.fp_cp) ) {
		//printf("4\n");
		return 0;
	}
	if ( BLOCKSIZE != ffs_fwrite(block, 1, BLOCKSIZE, ffs->tmp.fp_cp) ) {
		//printf("5\n");
		return 0;
	}
	
	pos = blockindex;
	pos *= BLOCKSIZE;
	ffs_fsetpos(ffs->fp, pos);
	U32toB4(cpindex, b4);
	if ( 4 != ffs_fwrite(b4, 1, 4, ffs->fp) ) {
		//printf("6\n");
		return 0; // 只更新fp中的cpindex
	}
	
	ffs->tmp.cp_size++;
	return 1;
}

static unsigned char removeblock(FileFS *ffs, unsigned int blockindex)
{
	if ( ffs->tmp.state == 0 ) return 0;
	
	// 读取block
	unsigned long long pos;
	unsigned int addindex;
	unsigned char buf[4], b4[4];
	unsigned int cpindex;
	unsigned int orgindex;
	unsigned char block[BLOCKSIZE];
	
	pos = blockindex;
	pos *= BLOCKSIZE;
	ffs_fsetpos(ffs->fp, pos);
	if ( 4 != ffs_fread(buf, 1, 4, ffs->fp) ) { // 超过了fp的文件尺寸，转为从fp_add中读取
		if ( blockindex < ffs->tmp.total_blocksize ) return 0;
		addindex = blockindex - ffs->tmp.total_blocksize;
		pos = addindex;
		pos *= (BLOCKSIZE+4);
		pos += 4 + 4; // 跳过前面的blockindex和cpindex
		ffs_fsetpos(ffs->tmp.fp_add, pos);
		U32toB4(ffs->tmp.new_unused_blockhead, b4);
		if ( 4 != ffs_fwrite(b4, 1, 4, ffs->tmp.fp_add) ) return 0; // 写入new_unused_blockhead
		ffs->tmp.new_unused_blockhead = blockindex; // 将blockindex存入new_unused_blockhead
		// printf("2.set new_unused_blockhead:%d\n", ffs->tmp.new_unused_blockhead);
		return 1;
	}
	
	// 此时tmp必然存在，且只能在fp_cp中，fp_add已经在前面处理过了，read by fp_cp
	memcpy(b4, buf, 4);
	cpindex = B4toU32(b4);
	if ( cpindex < ffs->tmp.cp_size ) {
		pos = cpindex;
		pos *= (BLOCKSIZE+4);
		ffs_fsetpos(ffs->tmp.fp_cp, pos);
		if ( 4 == ffs_fread(b4, 1, 4, ffs->tmp.fp_cp) ) {
			orgindex = B4toU32(b4);
			// 从fp中读取cpindex，再从指定的cpindex里读取blockindex
			// 比较2个blockindex是否相同，避免fp中写入的cpindex是错误的
			if ( orgindex == blockindex ) {
				pos += 8; // 跳过blockindex和cpindex
				ffs_fsetpos(ffs->tmp.fp_cp, pos);
				U32toB4(ffs->tmp.new_unused_blockhead, b4); // 写入blockindex(new_unused_blockhead)到fp_cp
				if ( 4 != ffs_fwrite(b4, 1, 4, ffs->tmp.fp_cp) ) return 0;
				ffs->tmp.new_unused_blockhead = blockindex; // 将blockindex存入new_unused_blockhead
				// printf("5.set new_unused_blockhead:%d\n", ffs->tmp.new_unused_blockhead);
				return 1;
			}
		}
	}

	cpindex = ffs->tmp.cp_size;
	
	pos = cpindex;
	pos *= (BLOCKSIZE+4);
	ffs_fsetpos(ffs->tmp.fp_cp, pos);
	U32toB4(blockindex, b4);
	if ( 4 != ffs_fwrite(b4, 1, 4, ffs->tmp.fp_cp) ) return 0;
	U32toB4(ffs->tmp.new_unused_blockhead, b4);
	memcpy(block+4, b4, 4); // 写入new_unused_blockhead
	pos += 4; // 跳过最前面的blockindex
	ffs_fsetpos(ffs->tmp.fp_cp, pos);
	if ( BLOCKSIZE != ffs_fwrite(block, 1, BLOCKSIZE, ffs->tmp.fp_cp) ) return 0;
	
	pos = blockindex;
	pos *= BLOCKSIZE;
	ffs_fsetpos(ffs->fp, pos);
	U32toB4(cpindex, b4);
	if ( 4 != ffs_fwrite(b4, 1, 4, ffs->fp) ) return 0; // 只更新fp中的cpindex
	
	ffs->tmp.cp_size++;
	
	ffs->tmp.new_unused_blockhead = blockindex; // 将blockindex存入new_unused_blockhead
	// printf("3.set new_unused_blockhead:%d\n", ffs->tmp.new_unused_blockhead);
	return 1;
}

// =======================================
static void j2ffs(FileFS *ffs)
{
	/*
		fnj: 
		blocksize(4 byte)
		state(byte,0xff-ready,other-no ready)
		block index 1(4 byte), block 1(BLOCKSIZE byte)
		block index 2(4 byte), block 2(BLOCKSIZE byte)
		...
		block index n(4 byte), block n(BLOCKSIZE byte) (n=blocksize)
	*/
	FILE *fpj;
	
	fpj = ffs_fopen(ffs->fnj, "rb");
	if ( fpj == NULL ) return;
	
	unsigned char b4[4];
	unsigned int blocksize;
	if ( 1 != ffs_fread(b4, 1, 4, fpj) ) {
		ffs_fclose(fpj);
		ffs_remove(ffs->fnj);
		return;
	}
	blocksize = B4toU32(b4);
	
	unsigned char state;
	if ( 1 != ffs_fread(&state, 1, 1, fpj) ) {
		ffs_fclose(fpj);
		ffs_remove(ffs->fnj);
		return;
	}
	if ( state != 0xff ) {
		ffs_fclose(fpj);
		ffs_remove(ffs->fnj);
		return;
	}
	
	unsigned int n = 0;
	unsigned char index_block[4 + BLOCKSIZE];
	unsigned int index;
	unsigned long long pos;
	while (1) {
		if ( 4+BLOCKSIZE != ffs_fread(index_block, 1, 4+BLOCKSIZE, fpj) ) break;
		
		memcpy(b4, index_block, 4);
		index = B4toU32(b4);
		pos = index;
		pos *= BLOCKSIZE;
		ffs_fsetpos(ffs->fp, pos);
		if ( BLOCKSIZE != ffs_fwrite(index_block+4, 1, BLOCKSIZE, ffs->fp) ) break;
		
		n++;
		if ( n >= blocksize ) break; 
	}

	ffs_fflush(ffs->fp);
	
	ffs_fclose(fpj);
	ffs_remove(ffs->fnj);
}

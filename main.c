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

#include "FileFS.h"

void usage(void)
{
	printf("  Supported commands:\n");
	printf("\t?/h/help\n");
	printf("\tq/quit\n");
	printf("\tmkfs fs_filename\n");
	printf("\tmount fs_filename\n");
	printf("\tunmount\n");
	printf("\tpwd\n");
	printf("\tls (path)\n");
	printf("\tcd path\n");
	printf("\tmkdir path\n");
	printf("\trm path\n");
	printf("\techo filename content\n");
	printf("\tadd filename content\n");
	printf("\tow filename content (overwrite file)\n");
	printf("\tcat filename\n");
	printf("\tfilesize filename\n");
	printf("\tseek\n");
	printf("\tdel filename\n");
	printf("\trename from to\n");
	printf("\tmv from to\n");
	printf("\tcp from to\n");
	printf("\tbegin\n");
	printf("\tcommit\n");
	printf("\trollback\n");
}

static void fun_ls(FileFS *ffs, char *path)
{
	if ( path == NULL ) return;
	int len = (int)strlen(path);
	if ( len < 1 ) return;
	
	char *sol_path;
	FFS_DIR *dirp;
	struct FFS_dirent *dir;

	int n_dir=0, n_file = 0;
	
	if ( NULL == (dirp = FileFS_opendir(ffs, path, &sol_path)) ) {
		printf("path ERR\n");
		return;
	}
	printf("  [dir]: %s\n", sol_path);
	while (1) {
		dir = FileFS_readdir(ffs, dirp);
		if ( dir == NULL ) break; // 当前目录为空
		
		if (strcmp(dir->d_name, ".") == 0) {
			if ( dir->d_type == FFS_DT_DIR ) {
				printf("\t<DIR>\t.\n");
			}
			continue;
		}
		if (strcmp(dir->d_name, "..") == 0) {
			if ( dir->d_type == FFS_DT_DIR ) printf("\t<DIR>\t..\n");
			continue;
		}
		
		// dir
		if ( dir->d_type == FFS_DT_DIR ) {
			printf("\t<DIR>\t%s\n", dir->d_name);
			n_dir++;
			continue;
		}
		
		// file, FFS_DT_FILE
		printf("\t\t%s\n", dir->d_name);
		n_file++;
	}
	FileFS_closedir(ffs, dirp);
	printf("  dir:%d, file:%d\n", n_dir, n_file);
}

static void fun_fwrite(FileFS *ffs, char *filename, char *content, char *mode)
{
	FFS_FILE *fp = FileFS_fopen(ffs, filename, mode);
	if ( fp == NULL ) {
		printf("fopen %s err\n", filename);
		return;
	}
	
	int r = (int)FileFS_fwrite(ffs, content, 1, (int)strlen(content), fp);
	printf("write %d to %s\n", r, filename);
	
	FileFS_fclose(ffs, fp);
}

static void fun_cat(FileFS *ffs, char *filename)
{
	FFS_FILE *fp = FileFS_fopen(ffs, filename, "r");
	if ( fp == NULL ) {
		printf("fopen %s err, not exist\n", filename);
		return;
	}
	
	char txt[1024];
	int r, n=0;
	
	while (1) {
		memset(txt, 0, 1024);	
		r = (int)FileFS_fread(ffs, txt, 1, 1023, fp);
		n += r;	
		if ( r > 0 ) printf("%s", txt);
		else break;
	}
	printf("\nread %d from %s\n", n, filename);
	
	FileFS_fclose(ffs, fp);
}

static void fun_filesize(FileFS *ffs, char *filename)
{
	FFS_FILE *fp = FileFS_fopen(ffs, filename, "a+");
	if ( fp == NULL ) {
		printf("fopen %s err, not exist\n", filename);
		return;
	}
	unsigned long long size = FileFS_ftell(ffs, fp);
	FileFS_fclose(ffs, fp);
	
	printf("file (%s) size:%I64d\n", filename, size);
}

static void fun_seek(FileFS *ffs, char *filename)
{
	char txt[1024];
	FFS_FILE *fp = FileFS_fopen(ffs, filename, "r+");
	if ( fp == NULL ) {
		printf("seek fopen %s err, not exist\n", filename);
		return;
	}
	
	FileFS_fseek(ffs, fp, 10, FFS_SEEK_CUR);
	//if ( ! FileFS_fseek(ffs, fp, -20, FFS_SEEK_END) ) {
	if ( ! FileFS_fseek(ffs, fp, 15, FFS_SEEK_SET) ) {
		printf("seek err\n");
	}
	sprintf(txt, ".....insert.....");
	FileFS_fwrite(ffs, txt, 1, (int)strlen(txt), fp);
	
	unsigned long long pos = FileFS_ftell(ffs, fp);
	printf("pos:%I64d\n", pos);
	
	FileFS_fclose(ffs, fp);
}

int main(int argc, char *argv[])
{	
	int done;
	char cmd[256], *fn, *path;
	int ret;
	int r;
	char filename[128], *txt;
	
	FileFS *ffs;
	
	ffs = FileFS_create();
	if ( ffs == NULL ) {
		printf("FileFS create ERR\n");
		return 0;
	}

	done = 0;

	printf("Welcome to FileFS Browsing Shell v1.0\n");
	while (!done) {
		printf("$>");
		// if ( ! FileFS_ismount(ffs) ) printf("$>");
		// else printf("%s>", FileFS_getcwd(ffs));
		ret = scanf("%[^\n]", cmd);
		if (ret < 0) {
			done = 1;
			printf("\n");
			continue;
		} else {
			getchar();
			if (ret == 0) continue;
		}
		if (strcmp(cmd, "?") == 0) {
			usage();
			continue;
		} else if (strcmp(cmd, "help") == 0) {
			usage();
			continue;
		} else if (strcmp(cmd, "h") == 0) {
			usage();
			continue;
		} else if (strcmp(cmd, "q") == 0) {
			done = 1;
			continue;
		} else if (strcmp(cmd, "quit") == 0) {
			done = 1;
			continue;
		} else if (strncmp(cmd, "mkfs", 4) == 0) {
			if (cmd[4] == ' ') {
				fn = cmd + 5;
				while (*fn == ' ') fn++;
				if (*fn != '\0') {
					if ( FileFS_mkfs(fn) ) printf("OK, mkfs %s\n", fn);
					else printf("ERR, mkfs %s\n", fn);
					continue;
				}
			}
		} else if (strncmp(cmd, "mount", 5) == 0) {
			if (cmd[5] == ' ') {
				fn = cmd + 6;
				while (*fn == ' ') fn++;
				if (*fn != '\0') {
					if ( FileFS_mount(ffs, fn) ) printf("OK, mount %s\n", fn);
					else printf("ERR, mount %s\n", fn);
					continue;
				}
			}
		} else if (strcmp(cmd, "umount") == 0) {
			FileFS_umount(ffs);
			continue;
		} else if (strcmp(cmd, "pwd") == 0) {
			if ( ! FileFS_ismount(ffs) ) {
				printf("ERR: not mount data file.\n");
			} else {
				printf("%s\n", FileFS_getcwd(ffs));
			}
			continue;
		} else if (strncmp(cmd, "ls", 2) == 0) {
			if ( ! FileFS_ismount(ffs) ) {
				printf("ERR: not mount data file.\n");
			} else {
				if (cmd[2] == ' ') {
					path = cmd + 3;
					while (*path == ' ') path++;
					if (*path != '\0') {
						// printf("path:%s\n", path);
						fun_ls(ffs, path);
						continue;
					}
				} else if (cmd[2] == 0 ) {
					fun_ls(ffs, ".");
				}
			}
			continue;
		} else if (strncmp(cmd, "cd", 2) == 0) {
			if (cmd[2] == ' ') {
				path = cmd + 3;
				while (*path == ' ') path++;
				if (*path != '\0') {
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						r = FileFS_chdir(ffs, path);
						if ( r == 0 ) {
							printf("cd %s ERR\n", path);
						}
					}
				}
			} else {
				if ( ! FileFS_ismount(ffs) ) {
					printf("ERR: not mount data file.\n");
				} else {
					r = FileFS_chdir(ffs, "/");
					if ( r == 0 ) {
						printf("cd / ERR\n");
					}
				}
			}
			continue;
		} else if (strncmp(cmd, "mkdir", 5) == 0) {
			if (cmd[5] == ' ') {
				path = cmd + 6;
				while (*path == ' ') path++;
				if (*path != '\0') {
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						r = FileFS_mkdir(ffs, path);
						// return: 0-ok,1-gen err,2-name>limit(14byte),3-dirtroy existed
						if ( r == 1 ) {
							printf("mkdir %s ERR\n", path);
						} else if ( r == 2 ) {
							printf("ERR: name too long [%s].\n", path);
						} else if ( r == 3 ) {
							printf("directory %s is existed.\n", path);
						} else if ( r == 4 ) {
							printf("exist same name file [%s].\n", path);
						}
					}
					continue;
				}
			}
		} else if (strncmp(cmd, "rm", 2) == 0) {
			if (cmd[2] == ' ') {
				path = cmd + 3;
				while (*path == ' ') path++;
				if (*path != '\0') {
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						r = FileFS_rmdir(ffs, path);
						// return: 0-ok,1-gen err,2-sub dir not empty,3-dirtroy not existed,4-name>limit(14byte)
						if ( r == 1 ) {
							printf("rmdir %s ERR\n", path);
						} else if ( r == 2 ) {
							printf("ERR: sub path not empty [%s].\n", path);
						} else if ( r == 3 ) {
							printf("ERR: path not exist [%s].\n", path);
						} else if ( r == 4 ) {
							printf("ERR: name to long [%s].\n", path);
						}
					}
					continue;
				}
			}
		} else if (strncmp(cmd, "echo", 4) == 0) {
			if (cmd[4] == ' ') {
				txt = cmd + 5;
				while (*txt == ' ') txt++;
				if (*txt != '\0') {
					fn = txt;
					while (*txt != ' ') txt++;
					memset(filename, 0, 128);
					memcpy(filename, fn, txt-fn);
					while (*txt == ' ') txt++;
					// printf("echo '%s' %s\n", txt, filename);
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						fun_fwrite(ffs, filename, txt, "w");
					}
					continue;
				}
			}
		} else if (strncmp(cmd, "add", 3) == 0) {
			if (cmd[3] == ' ') {
				txt = cmd + 4;
				while (*txt == ' ') txt++;
				if (*txt != '\0') {
					fn = txt;
					while (*txt != ' ') txt++;
					memset(filename, 0, 128);
					memcpy(filename, fn, txt-fn);
					while (*txt == ' ') txt++;
					// printf("echo '%s' %s\n", txt, filename);
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						fun_fwrite(ffs, filename, txt, "a");
					}
					continue;
				}
			}
		} else if (strncmp(cmd, "ow", 2) == 0) {
			if (cmd[2] == ' ') {
				txt = cmd + 3;
				while (*txt == ' ') txt++;
				if (*txt != '\0') {
					fn = txt;
					while (*txt != ' ') txt++;
					memset(filename, 0, 128);
					memcpy(filename, fn, txt-fn);
					while (*txt == ' ') txt++;
					// printf("echo '%s' %s\n", txt, filename);
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						fun_fwrite(ffs, filename, txt, "r+");
					}
					continue;
				}
			}
		} else if (strncmp(cmd, "cat", 3) == 0) {
			if (cmd[3] == ' ') {
				fn = cmd + 4;
				while (*fn == ' ') fn++;
				if (*fn != '\0') {
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						fun_cat(ffs, fn);
					}
					continue;
				}
			}
		} else if (strncmp(cmd, "filesize", 8) == 0) {
			if (cmd[8] == ' ') {
				fn = cmd + 9;
				while (*fn == ' ') fn++;
				if (*fn != '\0') {
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						fun_filesize(ffs, fn);
					}
					continue;
				}
			}
		} else if (strncmp(cmd, "seek", 4) == 0) {
			if (cmd[4] == ' ') {
				fn = cmd + 5;
				while (*fn == ' ') fn++;
				if (*fn != '\0') {
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						fun_seek(ffs, fn);
					}
					continue;
				}
			}
			continue;
		} else if (strncmp(cmd, "del", 3) == 0) {
			if (cmd[3] == ' ') {
				fn = cmd + 4;
				while (*fn == ' ') fn++;
				if (*fn != '\0') {
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						// return: 0-ok,1-gen err,2-file not exist,3-dir not existed,4-name>limit(14byte),5-name format err
						r = FileFS_remove(ffs, fn);
						if ( r == 1 ) {
							printf("remove %s ERR\n", fn);
						} else if ( r == 2 ) {
							printf("ERR: file not exist [%s].\n", fn);
						} else if ( r == 3 ) {
							printf("ERR: dir not exist [%s].\n", fn);
						} else if ( r == 4 ) {
							printf("ERR: name to long [%s].\n", fn);
						} else if ( r == 5 ) {
							printf("ERR: name format err [%s].\n", fn);
						}
					}
					continue;
				}
			}
		} else if (strncmp(cmd, "rename", 6) == 0) {
			if (cmd[6] == ' ') {
				txt = cmd + 7;
				while (*txt == ' ') txt++;
				if (*txt != '\0') {
					fn = txt;
					while (*txt != ' ') txt++;
					memset(filename, 0, 128);
					memcpy(filename, fn, txt-fn);
					while (*txt == ' ') txt++;
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						// return: 0:ok,1-err,2-old name format err,3-new name format err,4-old name not exist,5-new name exist, 6-old new format not match
						r = FileFS_rename(ffs, filename, txt);
						if ( r == 1 ) {
							printf("rename %s ERR\n", fn);
						} else if ( r == 2 ) {
							printf("ERR: old name format err [%s].\n", filename);
						} else if ( r == 3 ) {
							printf("ERR: new name format err [%s].\n", txt);
						} else if ( r == 4 ) {
							printf("ERR: old name not exist [%s].\n", filename);
						} else if ( r == 5 ) {
							printf("ERR: new name exist [%s].\n", txt);
						} else if ( r == 6 ) {
							printf("ERR: old new format not match [%s].\n", fn);
						}
					}
					continue;
				}
			}
		} else if (strncmp(cmd, "mv", 2) == 0) {
			if (cmd[2] == ' ') {
				txt = cmd + 3;
				while (*txt == ' ') txt++;
				if (*txt != '\0') {
					fn = txt;
					while (*txt != ' ') txt++;
					memset(filename, 0, 128);
					memcpy(filename, fn, txt-fn);
					while (*txt == ' ') txt++;
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						// return: 0:ok,1-err,2-from name format err,3-to path format err,4-from name not exist,5-to file exist, 6-from to format not match
						r = FileFS_move(ffs, filename, txt);
						if ( r == 1 ) {
							printf("mv %s ERR\n", fn);
						} else if ( r == 2 ) {
							printf("ERR: from name format err [%s].\n", filename);
						} else if ( r == 3 ) {
							printf("ERR: to path format err [%s].\n", txt);
						} else if ( r == 4 ) {
							printf("ERR: from name not exist [%s].\n", filename);
						} else if ( r == 5 ) {
							printf("ERR: to file exist [%s].\n", txt);
						} else if ( r == 6 ) {
							printf("ERR: from to format not match [%s].\n", fn);
						}
					}
					continue;
				}
			}
		} else if (strncmp(cmd, "cp", 2) == 0) {
			if (cmd[2] == ' ') {
				txt = cmd + 3;
				while (*txt == ' ') txt++;
				if (*txt != '\0') {
					fn = txt;
					while (*txt != ' ') txt++;
					memset(filename, 0, 128);
					memcpy(filename, fn, txt-fn);
					while (*txt == ' ') txt++;
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						// return: 0:ok,1-err,2-from name format err,3-to path format err,4-from name not exist,5-to file exist
						r = FileFS_copy(ffs, filename, txt);
						if ( r == 1 ) {
							printf("copy %s ERR\n", fn);
						} else if ( r == 2 ) {
							printf("ERR: from name format err [%s].\n", filename);
						} else if ( r == 3 ) {
							printf("ERR: to path format err [%s].\n", txt);
						} else if ( r == 4 ) {
							printf("ERR: from name not exist [%s].\n", filename);
						} else if ( r == 5 ) {
							printf("ERR: to file exist [%s].\n", txt);
						}
					}
					continue;
				}
			}
		} else if (strcmp(cmd, "begin") == 0) {
			if ( ! FileFS_ismount(ffs) ) {
				printf("ERR: not mount data file.\n");
			} else {
				if ( ! FileFS_begin(ffs) ) printf("begin err\n");
			}
			continue;
		} else if (strcmp(cmd, "commit") == 0) {
			if ( ! FileFS_ismount(ffs) ) {
				printf("ERR: not mount data file.\n");
			} else {
				if ( ! FileFS_commit(ffs) ) printf("commit err\n");
			}
			continue;
		} else if (strcmp(cmd, "rollback") == 0) {
			if ( ! FileFS_ismount(ffs) ) {
				printf("ERR: not mount data file.\n");
			} else {
				FileFS_rollback(ffs);
			}
			continue;
		}
		printf("  Unknown/Incorrect command: %s\n", cmd);
		usage();
	}
	
	FileFS_destroy(ffs);
	
	return 0;
}

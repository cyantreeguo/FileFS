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
	printf("\ttree\n");
	printf("\tusermod path\n");
	printf("\tmkdir path\n");
	printf("\trm path\n");
	printf("\tfrm path(rmdir path recursively)\n");
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
	printf("\tincp file_out to_in(copy file_outof_filefs to file_inof_filefs)\n");
	printf("\toutcp from_in to_out(copy file_inof_filefs to file_outof_filefs)\n");
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

static unsigned char ffs_rmdir(FileFS *ffs, char *path)
{
	if (path == NULL) return 0;
	int len = (int)strlen(path);
	if (len < 1) return 0;

	char *srcpath = NULL, *runpath = NULL, c;
	int runlen = 0;
	FFS_dirent *dir;
	FFS_DIR *dirp;
	int i;
	char *filename = NULL;

	if ( ! FileFS_chdir(ffs, path) ) return 0;
	
	char *rootpath;
	char *s = FileFS_getcwd(ffs);
	rootpath = (char*)malloc((int)strlen(s) + 1);
	if ( rootpath == NULL ) return 0;
	strcpy(rootpath, s);
	
	char *abs_path, sub_dir[20];
	unsigned char path_empty;
	while (1) {
		path_empty = 1;
		
		dirp = FileFS_opendir(ffs, ".", &abs_path);
		if (dirp == NULL) return 0;
		// printf("search path:%s\n", abs_path);
		while (1) {
			dir = FileFS_readdir(ffs, dirp);
			if (dir == NULL) break;
			
			if ( strcmp(dir->d_name, ".") == 0 ) continue;
			if ( strcmp(dir->d_name, "..") == 0 ) continue;
			
			if ( path_empty > 0 ) path_empty = 2;
			if ( dir->d_type == FFS_DT_FILE ) {
				FileFS_remove(ffs, dir->d_name);
				// printf("remove %s\n", dir->d_name);
			} else {
				// printf("rmdir %s\n", dir->d_name);
				if ( 0 != FileFS_rmdir(ffs, dir->d_name) ) {
					if ( path_empty > 0 ) {
						path_empty = 0;
						strcpy(sub_dir, dir->d_name);
					}
				}
			}
		}
		FileFS_closedir(ffs, dirp);

		if ( path_empty == 0 ) {
			if ( ! FileFS_chdir(ffs, sub_dir) ) return 0;
		} else if ( path_empty == 1 ) {
			s = FileFS_getcwd(ffs);
			if ( strcmp(s, rootpath) == 0 ) {
				if ( ! FileFS_chdir(ffs, "..") ) return 0;
				break;
			} else {
				if ( ! FileFS_chdir(ffs, "..") ) return 0;
			}
		}
	}
	
	free(rootpath);
	
	int r = FileFS_rmdir(ffs, path);
	if ( 0 == r || 3 == r/*path not exist*/ ) return 1;
	return 0;
}
static void fun_forcerm(FileFS *ffs, char *path)
{
	if ( ! ffs_rmdir(ffs, path) ) printf("rmdir err\n");
}

typedef struct Tree Tree;
typedef struct Tree {
	char name[15];
	Tree *sub, *parent;
	Tree *prev, *next;
} Tree;
static unsigned char search(FileFS *ffs, char *path, Tree *tree_parent, int *count, Tree **treehead)
{
	char *sol_path;
	FFS_DIR *dirp;
	struct FFS_dirent *dir;
	Tree *tree, *tree_prev = NULL, *tree1 = NULL;
	int dircount = 0;
	
	// printf("search path:%s\n", FileFS_getcwd(ffs));
	if ( NULL == (dirp = FileFS_opendir(ffs, path, &sol_path)) ) return 0;
	while (1) {
		dir = FileFS_readdir(ffs, dirp);
		if ( dir == NULL ) break; // 当前目录为空
		
		if (strcmp(dir->d_name, ".") == 0) {
			continue;
		}
		if (strcmp(dir->d_name, "..") == 0) {
			continue;
		}
		
		// dir
		if ( dir->d_type == FFS_DT_DIR ) {
			// printf(" |_%s\n", dir->d_name);
			tree = (Tree*)malloc(sizeof(Tree));
			if ( tree == NULL ) {
				printf("malloc err\n");
				exit(0);
			}
			if ( tree1 == NULL ) tree1 = tree;
			memset(tree, 0, sizeof(Tree));
			strcpy(tree->name, dir->d_name);
			tree->parent = tree_parent;
			tree->prev = tree_prev;
			if ( tree_prev != NULL ) tree_prev->next = tree;
			//if ( treehead == NULL ) treehead = tree;
			tree_prev = tree;
			dircount++;
			continue;
		}
	}
	FileFS_closedir(ffs, dirp);
	
	*count = dircount;
	*treehead = tree1;
	return 1;
}
static void fun_tree(FileFS *ffs)
{
	char *path = FileFS_getcwd(ffs);
	int len = (int)strlen(path);
	if ( len < 1 ) return;
	
	char *path_root = strdup(path);
	
	Tree *treehead = NULL, *tree, *tree_parent;
	int dircount;
	Tree *tree_sub;

	tree_parent = NULL;
	if ( ! search(ffs, path, tree_parent, &dircount, &tree) ) {
		free(path_root);
		return;
	}
	treehead = tree;
	unsigned char flag;
	while (1) {
		if ( dircount == 0 ) break;
		FileFS_chdir(ffs, tree->name);
		tree_parent = tree;
		if ( ! search(ffs, ".", tree_parent, &dircount, &tree) ) {
			free(path_root);
			return;
		}
		tree_parent->sub = tree;
		if ( dircount > 0 ) continue;
		
		// dircount == 0;
		tree = tree_parent;
		FileFS_chdir(ffs, "..");
		flag = 0;
		while (1) {
			if ( tree->next != NULL ) {
				tree = tree->next;
				break;
			}

			if (tree->parent == NULL) {
				flag = 1;
				break;
			}
			tree = tree->parent;
			FileFS_chdir(ffs, "..");
		}
		if ( flag == 1 ) break;
		dircount = 1;
	}
	
	// show tree
	char space[1024];
	memset(space, 0, 1024);
	tree = treehead;
	if ( tree == NULL ) {
		free(path_root);
		return;
	}
	tree_parent = NULL;
	while (1) {
		printf("%s|_%s\n", space, tree->name);
		if ( tree->sub != NULL ) {
			tree = tree->sub;
			strcat(space, "| ");
			continue;
		}
		if ( tree->next != NULL ) {
			tree = tree->next;
			continue;
		}
		flag = 0;
		while (1) {
			if ( tree->parent == NULL ) {
				flag = 1;
				break;
			}
			tree_sub = tree;
			tree = tree->parent;
			if ( space[0] != 0 ) space[strlen(space)-1] = 0;
			if ( space[0] != 0 ) space[strlen(space)-1] = 0;
			if ( tree->next != NULL ) {
				tree = tree->next;
				break;
			}
		}
		if ( flag == 1 ) break;
	}
	
	// free tree
	tree = treehead;
	if ( tree == NULL ) {
		free(path_root);
		return;
	}
	tree_parent = NULL;
	while (1) {
		if ( tree->sub != NULL ) {
			tree = tree->sub;
			continue;
		}
		if ( tree->next != NULL ) {
			tree = tree->next;
			continue;
		}
		flag = 0;
		while (1) {
			if ( tree->parent == NULL ) {
				flag = 1;
				break;
			}
			tree_sub = tree;
			tree = tree->parent;
			free(tree_sub);
			if ( tree->next != NULL ) {
				tree = tree->next;
				break;
			}
		}
		if ( flag == 1 ) break;
	}
	
	free(path_root);
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

static void fun_in_cp(FileFS *ffs, char *from_out, char *to_in)
{
	FILE *fp;
	
	fp = fopen(from_out, "rb");
	if ( fp == NULL ) {
		printf("err: can not read from_out(%s)\n", from_out);
		return;
	}
	
	FFS_FILE *ffp;
	ffp = FileFS_fopen(ffs, to_in, "w");
	if ( ffp == NULL ) {
		printf("err: can not create to_in(%s)\n", to_in);
	}
	
	unsigned char buf[1024];
	int len;
	
	while (1) {
		len = (int)fread(buf, 1, 1024, fp);
		if ( len != 1024 ) {
			if ( len > 0 ) {
				FileFS_fwrite(ffs, buf, 1, len, ffp);
				break;
			}
		}
		FileFS_fwrite(ffs, buf, 1, len, ffp);
	}
	
	FileFS_fclose(ffs, ffp);
	fclose(fp);
}

static void fun_out_cp(FileFS *ffs, char *from_in, char *to_out)
{
	FFS_FILE *ffp;
	ffp = FileFS_fopen(ffs, from_in, "r");
	if ( ffp == NULL ) {
		printf("err: can not create from_in(%s)\n", from_in);
	}
	
	FILE *fp;
	fp = fopen(to_out, "wb");
	if ( fp == NULL ) {
		printf("err: can not read to_out(%s)\n", to_out);
		return;
	}	
	
	unsigned char buf[1024];
	int len;
	
	while (1) {
		len = (int)FileFS_fread(ffs, buf, 1, 1024, ffp);
		if ( len != 1024 ) {
			if ( len > 0 ) {
				fwrite(buf, 1, len, fp);
				break;
			}
		}
		fwrite(buf, 1, len, fp);
	}
	
	fclose(fp);
	FileFS_fclose(ffs, ffp);
}

static void fun_cp(FileFS *ffs, char *from, char *to)
{
	FFS_FILE *ffp;
	ffp = FileFS_fopen(ffs, from, "r");
	if ( ffp == NULL ) {
		printf("err: can not create from(%s)\n", from);
	}
	
	FFS_FILE *fp;
	fp = FileFS_fopen(ffs, to, "w");
	if ( fp == NULL ) {
		printf("err: can not read to(%s)\n", to);
		return;
	}	
	
	unsigned char buf[1024];
	int len;
	//int n = 0, loop=0;
	
	while (1) {
		len = (int)FileFS_fread(ffs, buf, 1, 1024, ffp);
		if ( len != 1024 ) {
			if ( len > 0 ) {
				FileFS_fwrite(ffs, buf, 1, len, fp);
				//n += len;
				//loop++;
				//printf("last, len=%d, n=%d, loop=%d\n", len, n, loop);
				break;
			}
		}
		FileFS_fwrite(ffs, buf, 1, len, fp);
		//n += len;
		//loop++;
		//printf("len=%d, n=%d, loop=%d\n", len, n, loop);
	}
	
	FileFS_fclose(ffs, fp);
	FileFS_fclose(ffs, ffp);
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
					r = FileFS_chdir(ffs, "~");
					if ( r == 0 ) {
						printf("cd / ERR\n");
					}
				}
			}
			continue;
		} else if (strcmp(cmd, "tree") == 0) {
			if ( ! FileFS_ismount(ffs) ) {
				printf("ERR: not mount data file.\n");
			} else {
				fun_tree(ffs);
			}
			continue;
		} else if (strncmp(cmd, "usermod", 7) == 0) {
			if (cmd[7] == ' ') {
				path = cmd + 8;
				while (*path == ' ') path++;
				if (*path != '\0') {
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						r = FileFS_sethome(ffs, path);
						if ( r == 0 ) {
							printf("set home directory %s ERR\n", path);
						}
					}
				}
			} else {
				if ( ! FileFS_ismount(ffs) ) {
					printf("ERR: not mount data file.\n");
				} else {
					path = FileFS_gethome(ffs);
					printf("home directory: %s\n", path);
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
		} else if (strncmp(cmd, "frm", 3) == 0) {
			if (cmd[3] == ' ') {
				path = cmd + 4;
				while (*path == ' ') path++;
				if (*path != '\0') {
					if ( ! FileFS_ismount(ffs) ) {
						printf("ERR: not mount data file.\n");
					} else {
						fun_forcerm(ffs, path);
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
						//fun_cp(ffs, filename, txt);
						//*
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
						//*/
					}
					continue;
				}
			}
		} else if (strncmp(cmd, "incp", 4) == 0) {
			if (cmd[4] == ' ') {
				txt = cmd + 5;
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
						fun_in_cp(ffs, filename, txt);
					}
				}
			}
			continue;
		} else if (strncmp(cmd, "outcp", 5) == 0) {
			if (cmd[5] == ' ') {
				txt = cmd + 6;
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
						fun_out_cp(ffs, filename, txt);
					}
				}
			}
			continue;
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
		usage();
		printf("  Unknown/Incorrect command: %s\n", cmd);
	}
	
	FileFS_destroy(ffs);
	
	return 0;
}

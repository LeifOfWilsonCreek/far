#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <grp.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <pwd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "trav.h"

TRAV *trav_init(const char *dir, int flags) {
	TRAV *tree;

	if(!(tree = malloc(sizeof(*tree)))) goto err0;
	memset(tree, 0, sizeof(*tree));
	tree->flags = flags;
	tree->stacksize = 128;
	tree->first_run = 1;
	tree->base_size = strlen(dir);
	if(!(tree->dirstack = malloc(sizeof(DIR *) * tree->stacksize))) goto err1;
	if(!(tree->name = malloc((NAME_MAX+1) * tree->stacksize + tree->base_size + 2))) goto err2;
	strcpy(tree->name, dir);
	tree->name_comp = strlen(tree->name);
	if(tree->name[tree->name_comp-1] != '/') {
		tree->name[tree->name_comp++] = '/';
		tree->name[tree->name_comp] = 0;
		tree->base_size++;
	}
	if(!(tree->dirstack[0] = opendir(dir))) goto err3;
	return tree;

//err4:	closedir(tree->dirstack[0]);
err3:	free(tree->name);
err2:	free(tree->dirstack);
err1:	free(tree);
err0:	return 0;
}

void trav_free(TRAV *tree) {
	do {
		closedir(tree->dirstack[tree->stackbottom]);
	} while(tree->stackbottom--);
	free(tree->dirstack);
	free(tree->name);
	free(tree);
}

void trav_next(TRAV *tree) {
	struct dirent *entry;

	if(tree->eof) return;
	tree->error = 0;
	if(tree->first_run) {
		/* Kludge for making the starting dir act like the others... */
		tree->first_run = 0;
		if(tree->flags & TRAV_DIRFIRST) {
			tree->name[tree->name_comp - 1] = 0;
			if(fstat(dirfd(tree->dirstack[tree->stackbottom]), &tree->stat)) {
				/* Shouldn't happen. */
				tree->error = errno;
				tree->eof = 1;
			}
			return;
		}
	}
top:
	entry = readdir(tree->dirstack[tree->stackbottom]);
	if(!entry) {
		if(!tree->ret_dir && tree->flags & TRAV_DIRLAST) {
			/* Return the base name of this dir if requested */
			tree->name[tree->name_comp - 1] = 0;
			tree->ret_dir = 1;
			return;
		}
		tree->ret_dir = 0;

		/* End of directory - go up a level. */
		if(!tree->stackbottom) {
			/* top directory - end of traversal */
			tree->eof = 1;
			return;
		}

		closedir(tree->dirstack[tree->stackbottom]);
		tree->stackbottom--;

		/* Fix path name */
		tree->name_comp--;
		while(tree->name[--tree->name_comp] != '/');
		tree->name_comp++;

		goto top;
	}

	/* "." and ".." don't count */
	if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) goto top;

	tree->name[tree->name_comp - 1] = '/';
	strcpy(tree->name + tree->name_comp, entry->d_name);

	if(tree->flags & TRAV_FOLLOWSYM) {
		if(stat(tree->name, &tree->stat)) {
			if(tree->flags & TRAV_NOERROR)
				/* skip errors */
				goto top;
			tree->error = errno;
			return;
		}
	} else {
		if(lstat(tree->name, &tree->stat)) {
			if(tree->flags & TRAV_NOERROR)
				/* skip errors */
				goto top;
			tree->error = errno;
			return;
		}
	}


	/* Did we get a directory?  Recurse into it. */
	if(S_ISDIR(tree->stat.st_mode)) {
		void *ptr;

		if(tree->stackbottom + 1 >= tree->stacksize) {
			ptr = realloc(tree->dirstack, sizeof(DIR *) * (tree->stacksize+128));
			if(!ptr) {
				/* Out of memory is fatal */
				tree->error = errno;
				tree->eof = 1;
				return;
			}
			tree->dirstack = ptr;

			ptr = realloc(tree->name, (NAME_MAX+1) * (tree->stacksize+128) + tree->base_size + 2);
			if(!ptr) {
				/* Out of memory is fatal */
				tree->error = errno;
				tree->eof = 1;
				return;
			}
			tree->name = ptr;
			tree->stacksize += 128;
		}

		tree->stackbottom++;
		if(!(tree->dirstack[tree->stackbottom] = opendir(tree->name))) {
			tree->stackbottom--;
			if(tree->flags & TRAV_NOERROR)
				/* skip errors */
				goto top;
			tree->error = errno;
			return;
		}

		tree->name_comp = strlen(tree->name);
		tree->name[tree->name_comp++] = 0;
		if(tree->flags & TRAV_DIRFIRST)
			return;
		goto top;
	}

	/* If only dirs, get another */
	if(tree->flags & TRAV_DIRONLY) goto top;

	return;
}

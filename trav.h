/*
This library may be used to traverse a directory tree.  To use it, call
trav_init(), then pass the returned pointer into trav_next() to get each
filename one at a time.  When trav_init() returns, there are several
possibilites, based on items in TRAV:
 - TRAV.error == 0 && TRAV.eof == 0
Success.  The next filename is returned in TRAV.name.  TRAV.stat contains the
stat of TRAV.name.  Since stat() has to be called internally anyway, the result
is stored for the user for speed and convienence.
 - TRAV.error == 0 && TRAV.eof == 1
Successful completion.  The last file was already returned.  trav_free() should
now be called.
 - TRAV.error != 0 && TRAV.eof == 0
An error happened while trying to stat a file or recurse into a directory.  The
offending file is stored at TRAV.name and an appropriate errno is stored in
TRAV.error.  Further traversal is possible, but the offending file or directory
will be skipped.
 - TRAV.error != 0 && TRAV.eof == 1
A fatal error happened (like running out of memory).  Further traversal is not
possible.  An appropriate errno is stored in TRAV.error.  trav_free() should
now be called.  The most probable cause of this is a circular symlink (if they
are being followed) or an excessively deep directory structure.

Notes:
 - Do not change the current directory while a TRAV object exists.
 - This library is designed to be useful for utilities such as cp and far.  It
does not change the working directory.  It always returns the path to the next
file appended to the starting point specified.  ".." is never appended to the
initial path.  The returned string never ends in '/' and always contains the
initial part passed in, minus any trailing '/'.
 - It is safe to use more than one instantiation of at once in the same program
but trav_next() is not thread safe because it uses readdir() and errno.  It is
thread safe if you lock such that each call to trav_next() is atomic.
 - You may depend on reading from name, error, eof, and stat.  Do not modify
anything in the TRAV struct.
 - At initialization, you may pass in flags.  TRAV_DIRONLY indicates that only
directories should be returned.  One or both of the two following flags should
be used with TRAV_DIRONLY.  TRAV_DIRFIRST or TRAV_DIRLAST indicate that
directories should be returned before or after their contents.  Both may be
used, indicating that each directory should be returned exactly twice (once
before and once after).  TRAV_SYMLINK indicates that symbolic links should be
followed.  TRAV_NOERROR indicates that non-fatal errors should not be reported.
 - If a symlink is passed into trav_init(), it is always followed.
TRAV_SYMLINK applies to the actual traversal.

Sample code:
shows all files and directories on the filesystem and reports any errors:

int main() {
	TRAV *tr;

	tr = trav_init("/", TRAV_DIRFIRST);
	if(!tr) {
		perror("tr");
		exit(1);
	}
	while(1) {
		trav_next(tr);
		if(tr->eof)
			break;
		if(tr->error) {
			fprintf(stderr, "%s: %s\n", tr->name, strerror(tr->error));
			continue;
		}
		printf("%s\n", tr->name);
	}
	if(tr->error)
		fprintf(stderr, "Fatal error: %s\n", strerror(tr->error));
	trav_free(tr);
	return 0;
}
 */

#ifndef _TRAV_H
#define _TRAV_H

#define TRAV_DIRFIRST	0x01	/* Directories appear before their contents */
#define TRAV_DIRLAST	0x02	/* Directories appear after their contents */
#define TRAV_DIRONLY	0x04	/* Return only directores (no files) */
#define TRAV_FOLLOWSYM	0x08	/* Follow sym links */
#define TRAV_NOERROR	0x10	/* Ignore errors */

typedef struct {
	DIR **dirstack;		/* [0] is the first directory to be acted on */
	int stackbottom;	/* Index of the current dir in dirstack */
	int stacksize;		/* Max number of elems in dirstack */
	char *name;		/* File name of the element returned */
	int name_comp;		/* index of last component of name */
	int base_size;		/* strlen(starting dir) */
	int flags;		/* flags defined above for trav_init() */
	int error;		/* Stores errno on errors */
	int eof;		/* 1 if finished or fatally errored */
	struct stat stat;	/* stat of name */
	int ret_dir;		/* Return a dir or recurse out of it? */
	int first_run;		/* First run of trav_next()? */
} TRAV;

TRAV *trav_init(const char *dir, int flags);
void trav_next(TRAV *tree);
void trav_free(TRAV *tree);

#endif /* !_TRAV_H */

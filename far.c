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

#include "far.h"
#include "trav.h"

#ifndef COMPRESS
#define COMPRESS "/usr/bin/gzip"
#endif
#ifndef UNCOMPRESS
#define UNCOMPRESS "/usr/bin/gunzip"
#endif

// todo:
// handle hard links
// need "check" and "repair" actions
// fix extracting mod times on dirs?
// -I - include list file
// -qwX

/* Notes:
 - Most function return values mean:
	0 - sucessful, no errors.
	1 - error on a single file.  if(!stoperr) it kept going until EOF.
if(stoperr), it stopped at the header for the next file
	2 - fatal error on the archive fd (write error, bad magic, etc).
These error numbers should propagate up to main()'s return value.  2 overrides
1 overrides 0.
 - fullread, fullwrite, skipaheadr, skipaheadw, and copyfd are stand-alone
utility functions
 - Utility functions do not print error messages.  Other functions print error
messages as soon as they happen to be as specific as possible.
*/

/* Options */
char action = 0;
char usetext = 0;
char followsym = 0;
char uselocks = 0;
char insecure = 0;
char preserve = 0;
char verbose = 0;
char gzip = 0;
char stoperr = 0;
char completion = 0;
char *archname = 0;
char *compress = COMPRESS;
char *uncompress = UNCOMPRESS;

int append_things(int archfd, char **files);

/* Just like read(), but waits for a full buffer, EOF, or error */
int fullread(int fd, void *buf, size_t nbytes) {
	size_t total;
	ssize_t len;

	total = 0;
	while(total < nbytes) {
		len = read(fd, buf + total, nbytes - total);
		if(len == 0) return total;
		if(len == -1) return total ? total : -1;
		total += len;
	}
	return total;
}

/* Just like write(), but waits for a full buffer, EOF, or error */
int fullwrite(int fd, const void *buf, size_t nbytes) {
	size_t total;
	ssize_t len;

	total = 0;
	while(total < nbytes) {
		len = write(fd, buf + total, nbytes - total);
		if(len == 0) return total;
		if(len == -1) return total ? total : -1;
		total += len;
	}
	return total;
}

/* Skips nbytes of data in a file descriptor, using seek() if it is seekable
 * and read() if it is not. */
int skipaheadr(int fd, off_t nbytes) {
	ssize_t len;
	char buf[0x10000];

	if(lseek(fd, nbytes, SEEK_CUR) != -1)
		return 0;
	/* archfd is a pipe or something not seekable */
	while(nbytes) {
		len = sizeof(buf);
		if(len > nbytes)
			len = nbytes;
		len = read(fd, buf, len);
		if(len == -1)
			return 1;
		if(nbytes && !len) /* Premature EOF */
			return 1;
		nbytes -= len;
	}
	return 0;
}

/* Skips nbytes on a writable descriptor, either by seek()ing or by writing
 * nbytes of zeros to fd. */
int skipaheadw(int fd, off_t nbytes) {
	ssize_t len;
	off_t sz;
	char buf[0x10000];

	if((sz = lseek(fd, nbytes, SEEK_CUR)) != -1) {
		if(ftruncate(fd, sz))
			return 1;
		return 0;
	}

	/* archfd is a pipe or something not seekable */
	memset(buf, 0, sizeof(buf));
	while(nbytes) {
		len = sizeof(buf);
		if(len > nbytes)
			len = nbytes;
		len = write(fd, buf, len);
		if(len == -1)
			return 1;
		if(nbytes && !len) /* Premature EOF */
			return 1;
		nbytes -= len;
	}
	return 0;
}

/* Copies nbytes from fromfd to tofd and stores the number of bytes read in
 * nread and the number of bytes written in nwrote.  Null pointers may be
 * passed in to either or both if you don't care.
 * Returns:
 *  0 - success
 * -1 - read error
 * -2 - write error
 * At return time:
 * - nread == nwrote unless write error.
 * - If write error, nread >= nwrote.
 * - nwrote == nbytes unless EOF or error.
 */
int copyfd(int fromfd, int tofd, off_t nbytes, off_t *nread, off_t *nwrote) {
	ssize_t len1, len2;
	char copybuf[0x10000];
	off_t foo, bar, left = nbytes;
	int ret = 0;

	if(!nread) nread = &foo;
	if(!nwrote) nwrote = &bar;
	*nread = 0;
	*nwrote = 0;

	while(left) {
		if(completion)
			fprintf(stderr, "%3i%%\r", (int)(*nread * 100 / nbytes));
		if(left >= sizeof(copybuf))
			len1 = fullread(fromfd, copybuf, sizeof(copybuf));
		else
			len1 = fullread(fromfd, copybuf, left);
		if(len1 == -1) {
			ret = -1;
			break;
		}
		if(len1 == 0)
			break;
		*nread += len1;
		len2 = fullwrite(tofd, copybuf, len1);
		if(len2 > 0)
			*nwrote += len2;
		if(len2 < len1) {
			ret = -2;
			break;
		}
		left -= len1;
	}
	if(completion)
		fprintf(stderr, "    \r");
	return ret;
}

/* Returns true if item appears in list */
int inlist(char **list, char *item) {
	while(list[0]) {
		if(!strcmp(list[0], item)) return 1;
		list++;
	}
	return 0;
}

/* Appends a single file to the archive. */
int append_file(int archfd, const char *file) {
	int tmpint, filefd;
	struct flock flock;
	struct stat sb;
	struct far_header fhead;
	char *ptr;
	off_t tmpoff;

	if(verbose)
		fprintf(stderr, "%s\n", file);

	/* Open */
	if((filefd = open(file, followsym ? O_RDONLY : O_RDONLY|O_NOFOLLOW, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH)) == -1) {
		fprintf(stderr, "%s: open: %s\n", file, strerror(errno));
		return 1;
	}

	if(uselocks) {
		/* Lock */
		flock.l_start = 0;
		flock.l_len = 0;
		flock.l_type = F_RDLCK;
		flock.l_whence = SEEK_SET;
		if(fcntl(filefd, uselocks == 1 ? F_SETLK : F_SETLKW, &flock) == -1) {
			fprintf(stderr, "%s: lock: %s\n", file, strerror(errno));
			close(filefd);
			return 1;
		}
	}

	if(fstat(filefd, &sb) == -1) {
		fprintf(stderr, "%s: fstat: %s\n", file, strerror(errno));
		close(filefd);
		return 1;
	}

	if(!S_ISREG(sb.st_mode)) {
		/* Shouldn't happen */
		fprintf(stderr, "%s: unregular file passed into append_file!\n", file);
		close(filefd);
		return 1;
	}

	/* Fill in header */
	memset(&fhead, 0, sizeof(fhead));
	fhead.magic =	FAR_MAGIC;
	fhead.type =	FAR_T_REGULAR;
	fhead.perm =	sb.st_mode;
	fhead.uid =	sb.st_uid;
	fhead.gid =	sb.st_gid;
	fhead.modify_time =	sb.st_mtime;
	fhead.create_time =	sb.st_mtime;
	fhead.namesize = strlen(file) + 1;
	fhead.size =	sb.st_size;
	ptr = user_from_uid(sb.st_uid, 1);
	if(ptr && strlen(ptr) < sizeof(fhead.username))
		strcpy(fhead.username, ptr);
	ptr = group_from_gid(sb.st_gid, 1);
	if(ptr && strlen(ptr) < sizeof(fhead.groupname))
		strcpy(fhead.groupname, ptr);

	/* Write header */
	HOST_2_FAR_HEADER(fhead);
	if(fullwrite(archfd, &fhead, sizeof(fhead)) < sizeof(fhead)) {
		fprintf(stderr, "Write failed during %s: %s\n", file, strerror(errno));
		close(filefd);
		return 2;
	}

	/* Write filename */
	tmpint = strlen(file) + 1;

	if(tmpint > 0x10000) {
		fprintf(stderr, "Warning: %s has a name longer than 65535.\n", file);
	}
	if(fullwrite(archfd, file, tmpint) < tmpint) {
		fprintf(stderr, "Write failed during %s: %s\n", file, strerror(errno));
		close(filefd);
		return 2;
	}

	/* Copy file */
	tmpint = copyfd(filefd, archfd, sb.st_size, &tmpoff, 0);
	if(tmpint == -1) {
		fprintf(stderr, "%s: read: %s (padding added)\n", file, strerror(errno));
		goto shorter;
	}
	if(tmpint == -2) {
		fprintf(stderr, "Write failed during %s: %s\n", file, strerror(errno));
		close(filefd);
		return 2;
	}
	if(tmpoff < sb.st_size) {
		/* Shorter than expected */
		fprintf(stderr, "%s: read: Unexpected EOF (padding added)\n", file);
	shorter:
		close(filefd);
  		if(skipaheadw(archfd, sb.st_size - tmpoff)) {
			fprintf(stderr, "Write failed during %s: %s\n", file, strerror(errno));
			return 2;
		}
		return 1;
	}

	if(read(filefd, &tmpoff, 1) == 1) {
		/* Longer than expected */
		fprintf(stderr, "%s: got longer during read (truncated)\n", file);
		close(filefd);
		return 1;
	}
	close(filefd);

	return 0;
}

/* Appends a symlink to the archive. */
int append_symlink(int archfd, const char *file) {
	int tmpint, linksz;
	struct stat sb;
	struct far_header fhead;
	char linkbuf[0x10000], *ptr;

	if(verbose)
		fprintf(stderr, "%s\n", file);

	linksz = readlink(file, linkbuf, sizeof(linkbuf)-1);
	if(linksz == -1) {
		fprintf(stderr, "%s: readlink: %s\n", file, strerror(errno));
		return 1;
	}
	linkbuf[linksz++] = 0;

	if(lstat(file, &sb) == -1) {
		fprintf(stderr, "%s: lstat: %s\n", file, strerror(errno));
		return 1;
	}

	/* Fill in header */
	memset(&fhead, 0, sizeof(fhead));
	fhead.magic =	FAR_MAGIC;
	fhead.type =	FAR_T_SYMLINK;
	fhead.perm =	sb.st_mode;
	fhead.uid =	sb.st_uid;
	fhead.gid =	sb.st_gid;
	fhead.modify_time =	sb.st_mtime;
	fhead.create_time =	sb.st_mtime;
	fhead.namesize = strlen(file) + 1;
	fhead.size =	linksz;
	ptr = user_from_uid(sb.st_uid, 1);
	if(ptr && strlen(ptr) < sizeof(fhead.username))
		strcpy(fhead.username, ptr);
	ptr = group_from_gid(sb.st_gid, 1);
	if(ptr && strlen(ptr) < sizeof(fhead.groupname))
		strcpy(fhead.groupname, ptr);

	/* Write header */
	HOST_2_FAR_HEADER(fhead);
	if(fullwrite(archfd, &fhead, sizeof(fhead)) < sizeof(fhead)) {
		fprintf(stderr, "Write failed during %s: %s\n", file, strerror(errno));
		return 2;
	}

	/* Write filename */
	tmpint = strlen(file) + 1;

	if(tmpint > 0x10000) {
		fprintf(stderr, "Warning: %s has a name longer than 65535.\n", file);
	}
	if(fullwrite(archfd, file, tmpint) < tmpint) {
		fprintf(stderr, "Write failed during %s: %s\n", file, strerror(errno));
		return 2;
	}

	/* write symlink value */
	if(fullwrite(archfd, linkbuf, linksz) < linksz) {
		fprintf(stderr, "Write failed during %s: %s\n", file, strerror(errno));
		return 2;
	}

	return 0;
}

/* Archives a directory and its contents */
int append_dir(int archfd, const char *file) {
	TRAV *tr;
	int ret = 0, tmpint;
	char *strs[2];

	strs[1] = 0;
	if(!(tr = trav_init(file, followsym ? TRAV_DIRFIRST|TRAV_FOLLOWSYM : TRAV_DIRFIRST))) {
		fprintf(stderr, "%s: trav: %s\n", file, strerror(errno));
		return 1;
	}
	while(1) {
		trav_next(tr);
		if(tr->eof) {
			if(tr->error) {
				fprintf(stderr, "%s: trav: %s\n", file, strerror(tr->error));
				ret = 2;
			}
			break;
		}
		if(tr->error) {
			fprintf(stderr, "%s: trav: %s\n", tr->name, strerror(tr->error));
			ret = 1;
			if(stoperr)
				break;
			continue;
		}
		if(S_ISDIR(tr->stat.st_mode)) {
			struct far_header fhead;
			char *ptr;

			if(verbose)
				fprintf(stderr, "%s\n", tr->name);

			/* Fill in the header */
			memset(&fhead, 0, sizeof(fhead));
			fhead.magic = FAR_MAGIC;
			fhead.type = FAR_T_DIRECTORY;
			fhead.perm = tr->stat.st_mode;
			fhead.uid = tr->stat.st_uid;
			fhead.gid = tr->stat.st_gid;
			fhead.modify_time = tr->stat.st_mtime;
			fhead.create_time = tr->stat.st_mtime;
			fhead.namesize = strlen(tr->name) + 1;
			fhead.size = 0;

			ptr = user_from_uid(tr->stat.st_uid, 1);
			if(ptr && strlen(ptr) < sizeof(fhead.username))
				strcpy(fhead.username, ptr);
			ptr = group_from_gid(tr->stat.st_gid, 1);
			if(ptr && strlen(ptr) < sizeof(fhead.groupname))
				strcpy(fhead.groupname, ptr);

			/* Write header */
			HOST_2_FAR_HEADER(fhead);
			if(fullwrite(archfd, &fhead, sizeof(fhead)) < sizeof(fhead)) {
				fprintf(stderr, "Write failed during %s: %s\n", tr->name, strerror(errno));
				ret = 2;
				break;
			}

			/* Write filename */
			tmpint = strlen(tr->name) + 1;

			if(tmpint > 0x10000) {
				fprintf(stderr, "Warning: %s has a name longer than 65535.\n", tr->name);
			}
			if(fullwrite(archfd, tr->name, tmpint) < tmpint) {
				fprintf(stderr, "Write failed during %s: %s\n", tr->name, strerror(errno));
				ret = 2;
				break;
			}

			/* Directories have no data to append */

			continue;
		}

		strs[0] = tr->name;
		if((tmpint = append_things(archfd, strs))) {
			if(tmpint == 2) {
				ret = 2;
				break;
			}
			ret = 1;
			if(stoperr)
				break;
		}
	}
	trav_free(tr);
	return ret;
}

/* Appends a null-termed list of files or directories to an archive. */
int append_things(int archfd, char **files) {
	struct stat sb;
	int ret = 0, tmpint;

	if(!files)
		return 0;
	files--;
	while(*++files) {
		/* Check file type */
		if((followsym ? stat(*files, &sb) : lstat(*files, &sb)) == -1) {
			fprintf(stderr, "%s: stat: %s\n", *files, strerror(errno));
			if(stoperr)
				return 1;
			ret = 1;
			continue;
		}

		if(S_ISREG(sb.st_mode)) {
			if((tmpint = append_file(archfd, *files))) {
				if(tmpint == 2)
					return 2;
				if(stoperr)
					return 1;
				ret = 1;
			}
			continue;
		}

		if(S_ISDIR(sb.st_mode)) {
			/* append_dir() may call append_things() again, but not
			 * to store a directory.  This is as deep as it goes. */
			if((tmpint = append_dir(archfd, *files))) {
				if(tmpint == 2)
					return 2;
				if(stoperr)
					return 1;
				ret = 1;
			}
			continue;
		}

		if(S_ISLNK(sb.st_mode)) {
			if((tmpint = append_symlink(archfd, *files))) {
				if(tmpint == 2)
					return 2;
				if(stoperr)
					return 1;
				ret = 1;
			}
			continue;
		}

		fprintf(stderr, "%s: Unarchivable file type.\n", *files);
		if(stoperr)
			return 1;
		ret = 1;
	}
	return ret;
}

/* Extracts all the files in an archive */
int extract_things(int archfd, char **files) {
	struct far_header fhead;
	char namebuf[0x10000], databuf[0x10000], *ptr, *ptr2;
	int ret = 0, filefd, tmpint;
	off_t nread;
	struct utimbuf ub;

	while(1) {
		/* Read header */
		tmpint = fullread(archfd, &fhead, sizeof(fhead));
		if(tmpint == 0)
			return ret;
		if(tmpint == -1) {
			perror("read");
			return 2;
		}
		if(tmpint < sizeof(fhead)) {
			fprintf(stderr, "read: unexpected EOF\n");
			return 2;
		}

		/* Convert endian */
		FAR_2_HOST_HEADER(fhead);

		/* Check magic */
		if(fhead.magic != FAR_MAGIC) {
			fprintf(stderr, "invalid archive: bad magic\n");
			return 2;
		}

		if(fhead.namesize > sizeof(namebuf)) {
			fprintf(stderr, "file skipped: filename too long\n");
			goto skipa;
		}

		if(fhead.namesize < 2) {
			fprintf(stderr, "file skipped: null filename\n");
		skipa:
			if(skipaheadr(archfd, fhead.namesize + fhead.size)) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}
			if(stoperr)
				return 1;
			ret = 1;
			continue;
		}

		/* Read name */
		if((tmpint = fullread(archfd, namebuf, fhead.namesize)) < fhead.namesize) {
			fprintf(stderr, "read: unexpected EOF\n");
			return 2;
		}

		/* Safety checks */
		if(tmpint)
			namebuf[tmpint-1] = 0;
		else
			namebuf[0] = 0;

		if(!insecure) {
			ptr = namebuf;   /* read ptr */
			ptr2 = namebuf;  /* write ptr */

			/* Strip out "." and ".." components */
			while(1) {
				/* Top of loop is the beginning of a path component */

				/* Can't start with '/' or have extra '/'s */
				while(ptr[0] == '/')
					ptr++;

				/* Check for ".." */
				if(ptr[0] == '.' && ptr[1] == '.') {
					if(!ptr[2]) {
						/* Trailing ".." */
						break;
					}
					if(ptr[2] == '/') {
						/* Embedded ".." */
						ptr += 3;
						continue;
					}
				}

				/* Check for "." */
				if(ptr[0] == '.') {
					if(!ptr[1]) {
						/* Trailing "." */
						break;
					}
					if(ptr[1] == '/') {
						/* Embedded "." */
						ptr += 2;
						continue;
					}
				}

				/* Copy the rest of this component */
				while(ptr[0] && ptr[0] != '/')
					*ptr2++ = *ptr++;

				if(!ptr[0]) break;
				*ptr2++ = '/';
			}

			/* No trailing slash */
			if(ptr2 > namebuf && ptr2[-1] == '/')
				ptr2--;

			/* Finish */
			ptr2[0] = 0;

			/* Namebuf not null? */
			if(!namebuf[0]) {
				/* This may happen legitimately if someone
				 * archives "." or "..".  No big deal. */
				if(skipaheadr(archfd, fhead.size)) {
					fprintf(stderr, "read: unexpected EOF\n");
					return 2;
				}
				continue;
			}
		}

		/* At this point, we've read the header and the filename.  Next
		 * comes the data. */

		if(files[0] && !inlist(files, namebuf)) {
			/* A list was specified and this file is not in it. */
			if(skipaheadr(archfd, fhead.size)) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}
			continue;
		}

		switch(fhead.type) {
		  case FAR_T_DELETED:
			if(skipaheadr(archfd, fhead.size)) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}
			break;
		  case FAR_T_REGULAR:
			if(verbose)
				fprintf(stderr, "%s\n", namebuf);

			/* In case a malicious user trys to clobber a file by
			 * knowingly making a hard link from a file they know
			 * will be extracted to a file to clobber. */
			unlink(namebuf);

			if((filefd = open(namebuf, O_WRONLY|O_CREAT|O_EXCL|O_TRUNC, fhead.perm & ~(S_ISUID|S_ISGID|S_ISVTX))) == -1) {
				fprintf(stderr, "%s: open: %s\n", namebuf, strerror(errno));
				if(skipaheadr(archfd, fhead.size)) {
					fprintf(stderr, "read: unexpected EOF\n");
					return 2;
				}
				if(stoperr)
					return 1;
				ret = 1;
				break;
			}
			tmpint = copyfd(archfd, filefd, fhead.size, &nread, 0);
			if(tmpint == -1) {
				perror("read");
				close(filefd);
				return 2;
			}
			if(tmpint == -2) {
				fprintf(stderr, "%s: write: %s\n", namebuf, strerror(errno));
				close(filefd);
				if(skipaheadr(archfd, fhead.size - nread)) {
					fprintf(stderr, "read: unexpected EOF\n");
					return 2;
				}
				if(stoperr)
					return 1;
				ret = 1;
			} else if(nread < fhead.size) {
				fprintf(stderr, "read: unexpected EOF\n");
				close(filefd);
				return 2;
			}

			/* set mod time */
			ub.modtime = fhead.modify_time;
			ub.actime = time(NULL);
			if(utime(namebuf, &ub)) {
				fprintf(stderr, "%s: setting modification time: %s\n", namebuf, strerror(errno));
				if(stoperr)
					return 1;
				ret = 1;
				break;
			}

			if(preserve) {
				struct stat tmpsb;
				mode_t perm;

				/* If we don't get ownership right, don't let
				 * suid/sgid/svtx through. */
				perm = fhead.perm;
				if(fchmod(filefd, perm & ~(S_ISUID|S_ISGID|S_ISVTX))) {
					fprintf(stderr, "%s: chmod: %s\n", namebuf, strerror(errno));
					goto skipb;
				}
				if(usetext) {
					struct passwd *pwptr;
					struct group *grptr;

					if(fstat(filefd, &tmpsb)) {
						fprintf(stderr, "%s: fstat: %s\n", namebuf, strerror(errno));
						goto skipb;
					}
					if((pwptr = getpwnam(fhead.username))) {
						tmpsb.st_uid = pwptr->pw_uid;
					} else {
						fprintf(stderr, "%s: getpwnam(%s): No such user\n", namebuf, fhead.username);
						if(stoperr) {
							close(filefd);
							return 1;
						}
						ret = 1;
						perm &= ~S_ISUID;
					}
					if((grptr = getgrnam(fhead.groupname))) {
						tmpsb.st_gid = grptr->gr_gid;
					} else {
						fprintf(stderr, "%s: getgrnam(%s): No such group\n", namebuf, fhead.groupname);
						if(stoperr) {
							close(filefd);
							return 1;
						}
						ret = 1;
						perm &= ~S_ISGID;
					}
				} else {
					tmpsb.st_uid = fhead.uid;
					tmpsb.st_gid = fhead.gid;
				}
				if(fchown(filefd, tmpsb.st_uid, tmpsb.st_gid)) {
					fprintf(stderr, "%s: chown: %s\n", namebuf, strerror(errno));
					goto skipb;
				}


				/* Try to add suid/sgid/svtx if necessary now
				 * that we know ownerships are correct. */
				if(fchmod(filefd, perm)) {
					fprintf(stderr, "%s: chmod: %s\n", namebuf, strerror(errno));
				skipb:
					close(filefd);
					if(stoperr)
						return 1;
					ret = 1;
					break;
				}
			}
			close(filefd);
			break;
		  case FAR_T_DIRECTORY:
			if(verbose)
				fprintf(stderr, "%s\n", namebuf);

			/* Directories' data -should- be zero-length anyway... */
			if(fhead.size && skipaheadr(archfd, fhead.size)) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}

			/* Do the deed */
			if(mkdir(namebuf, fhead.perm & ~(S_ISUID|S_ISGID|S_ISVTX))) {
				fprintf(stderr, "%s: mkdir: %s\n", namebuf, strerror(errno));
				if(stoperr)
					return 1;
				ret = 1;
				break;
			}

// utime doesn't stick on dirs because it changes when we put something in them

			if(preserve) {
				struct stat tmpsb;
				mode_t perm;

				/* If we don't get ownership right, don't let
				 * suid/sgid/svtx through. */
				perm = fhead.perm;
				if(chmod(namebuf, perm & ~(S_ISUID|S_ISGID|S_ISVTX))) {
					fprintf(stderr, "%s: chmod: %s\n", namebuf, strerror(errno));
					goto skipc;
				}
				if(usetext) {
					struct passwd *pwptr;
					struct group *grptr;

					if(stat(namebuf, &tmpsb)) {
						fprintf(stderr, "%s: stat: %s\n", namebuf, strerror(errno));
						goto skipc;
					}
					if((pwptr = getpwnam(fhead.username))) {
						tmpsb.st_uid = pwptr->pw_uid;
					} else {
						fprintf(stderr, "%s: getpwnam(%s): No such user\n", namebuf, fhead.username);
						if(stoperr)
							return 1;
						ret = 1;
						perm &= ~S_ISUID;
					}
					if((grptr = getgrnam(fhead.groupname))) {
						tmpsb.st_gid = grptr->gr_gid;
					} else {
						fprintf(stderr, "%s: getgrnam(%s): No such group\n", namebuf, fhead.groupname);
						if(stoperr)
							return 1;
						ret = 1;
						perm &= ~S_ISGID;
					}
				} else {
					tmpsb.st_uid = fhead.uid;
					tmpsb.st_gid = fhead.gid;
				}
				if(chown(namebuf, tmpsb.st_uid, tmpsb.st_gid)) {
					fprintf(stderr, "%s: chown: %s\n", namebuf, strerror(errno));
					goto skipc;
				}


				/* Try to add suid/sgid/svtx if necessary now
				 * that we know ownerships are correct. */
				if(chmod(namebuf, perm)) {
					fprintf(stderr, "%s: chmod: %s\n", namebuf, strerror(errno));
				skipc:
					if(stoperr)
						return 1;
					ret = 1;
					break;
				}
			}
			break;
		  case FAR_T_LINK:
			if(verbose)
				fprintf(stderr, "%s\n", namebuf);
fprintf(stderr, "Skipping hard link %s\n", namebuf);
			if(skipaheadr(archfd, fhead.size)) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}
			break;
		  case FAR_T_SYMLINK:
			if(verbose)
				fprintf(stderr, "%s\n", namebuf);

			/* Not too long? */
			if(fhead.size > sizeof(databuf)) {
				fprintf(stderr, "%s: symlink destination name too long\n", namebuf);
				if(skipaheadr(archfd, fhead.size)) {
					fprintf(stderr, "read: unexpected EOF\n");
					return 2;
				}
				if(stoperr)
					return 1;
				ret = 1;
				break;
			}

			/* Read link's data */
			if(fullread(archfd, databuf, fhead.size) < fhead.size) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}

			/* Safety check */
			if(fhead.size)
				databuf[fhead.size-1] = 0;
			else
				databuf[0] = 0;

			/* Do the deed */
			if(symlink(databuf, namebuf)) {
				fprintf(stderr, "%s: symlink to %s: %s\n", namebuf, databuf, strerror(errno));
				if(stoperr)
					return 1;
				ret = 1;
				break;
			}

			/* utime doesn't affect symlinks */

			if(preserve) {
				struct stat tmpsb;

				if(usetext) {
					struct passwd *pwptr;
					struct group *grptr;

					if(lstat(namebuf, &tmpsb)) {
						fprintf(stderr, "%s: lstat: %s\n", namebuf, strerror(errno));
						break;
					}
					if((pwptr = getpwnam(fhead.username))) {
						tmpsb.st_uid = pwptr->pw_uid;
					} else {
						fprintf(stderr, "%s: getpwnam(%s): No such user\n", namebuf, fhead.username);
						if(stoperr)
							return 1;
						ret = 1;
					}
					if((grptr = getgrnam(fhead.groupname))) {
						tmpsb.st_gid = grptr->gr_gid;
					} else {
						fprintf(stderr, "%s: getgrnam(%s): No such group\n", namebuf, fhead.groupname);
						if(stoperr)
							return 1;
						ret = 1;
					}
				} else {
					tmpsb.st_uid = fhead.uid;
					tmpsb.st_gid = fhead.gid;
				}
				if(lchown(namebuf, tmpsb.st_uid, tmpsb.st_gid)) {
					fprintf(stderr, "%s: lchown: %s\n", namebuf, strerror(errno));
					if(stoperr)
						return 1;
					ret = 1;
					break;
				}
			}
			break;
		  default:
			if(verbose)
				fprintf(stderr, "%s\n", namebuf);
			fprintf(stderr, "Skipping %s: unknown file type %i\n", namebuf, fhead.type);
			if(skipaheadr(archfd, fhead.size)) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}
			break;
		}
	}
}

/* Lists all the files in an archive */
int list_things(int archfd, char **files) {
	struct far_header fhead;
	char namebuf[0x10000];
	char linkbuf[0x10004];	/* Extra space for " => " */
	char modebuf[16];
	mode_t tmpmode;
	char mtbuf[32], ztbuf[32], *ptr, *ptr2;
	time_t tmptime;
	int ret = 0, tmpint;

	while(1) {
		/* Read header */
		if(fullread(archfd, &fhead, sizeof(fhead)) < sizeof(fhead))
			return ret;

		/* Convert endian */
		FAR_2_HOST_HEADER(fhead);

		/* Check magic */
		if(fhead.magic != FAR_MAGIC) {
			fprintf(stderr, "invalid archive: bad magic\n");
			return 2;
		}

		/* Read name */
		if(fhead.namesize > sizeof(namebuf)) {
			fprintf(stderr, "file skipped: filename too long\n");
			if(stoperr)
				return 1;
			ret = 1;
			if(skipaheadr(archfd, fhead.namesize + fhead.size)) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}
			continue;
		}
		if((tmpint = fullread(archfd, namebuf, fhead.namesize)) < fhead.namesize) {
			fprintf(stderr, "read: unexpected EOF\n");
			return 2;
		}

		/* Safety check */
		if(tmpint)
			namebuf[tmpint-1] = 0;
		else
			namebuf[0] = 0;


		if(!insecure) {
			ptr = namebuf;   /* read ptr */
			ptr2 = namebuf;  /* write ptr */

			/* Strip out "." and ".." components */
			while(1) {
				/* Top of loop is the beginning of a path component */

				/* Can't start with '/' or have extra '/'s */
				while(ptr[0] == '/')
					ptr++;

				/* Check for ".." */
				if(ptr[0] == '.' && ptr[1] == '.') {
					if(!ptr[2]) {
						/* Trailing ".." */
						break;
					}
					if(ptr[2] == '/') {
						/* Embedded ".." */
						ptr += 3;
						continue;
					}
				}

				/* Check for "." */
				if(ptr[0] == '.') {
					if(!ptr[1]) {
						/* Trailing "." */
						break;
					}
					if(ptr[1] == '/') {
						/* Embedded "." */
						ptr += 2;
						continue;
					}
				}

				/* Copy the rest of this component */
				while(ptr[0] && ptr[0] != '/')
					*ptr2++ = *ptr++;

				if(!ptr[0]) break;
				*ptr2++ = '/';
			}

			/* No trailing slash */
			if(ptr2 > namebuf && ptr2[-1] == '/')
				ptr2--;

			/* Finish */
			ptr2[0] = 0;

			/* Namebuf not null? */
			if(!namebuf[0]) {
				/* This may happen legitimately if someone
				 * archives "." or "..".  No big deal. */
				if(skipaheadr(archfd, fhead.size)) {
					fprintf(stderr, "read: unexpected EOF\n");
					return 2;
				}
				continue;
			}
		}

		/* At this point, we've read the header and the filename.  Next
		 * comes the data. */

		if(files[0] && !inlist(files, namebuf)) {
			/* A list was specified and this file is not in it. */
			if(skipaheadr(archfd, fhead.size)) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}
			continue;
		}


		linkbuf[0] = 0;
		switch(fhead.type) {
		  case FAR_T_DELETED:
			/* Skip data */
			if(skipaheadr(archfd, fhead.size)) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}

			if(!verbose)
				continue;

			strcpy(modebuf, " (deleted) ");
			break;
		  case FAR_T_REGULAR:
			/* Skip data */
			if(skipaheadr(archfd, fhead.size)) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}

			tmpmode = S_IFREG | fhead.perm;
			strmode(tmpmode, modebuf);
			break;
		  case FAR_T_DIRECTORY:
			/* Skip data */
			if(skipaheadr(archfd, fhead.size)) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}

			tmpmode = S_IFDIR | fhead.perm;
			strmode(tmpmode, modebuf);
			break;
		  case FAR_T_LINK:
			if(fhead.size > sizeof(linkbuf) - 4) {
				strcpy(linkbuf, " -> (too long)");

				/* Skip data */
				if(skipaheadr(archfd, fhead.size)) {
					fprintf(stderr, "read: unexpected EOF\n");
					return 2;
				}
			}

			strcpy(linkbuf, " -> ");
			if((tmpint = fullread(archfd, linkbuf+4, fhead.size)) < fhead.size) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}
			linkbuf[tmpint + 3] = 0;

			tmpmode = S_IFREG | fhead.perm;
			strmode(tmpmode, modebuf);
			break;
		  case FAR_T_SYMLINK:
			if(fhead.size > sizeof(linkbuf) - 4) {
				strcpy(linkbuf, " => (too long)");

				/* Skip data */
				if(skipaheadr(archfd, fhead.size)) {
					fprintf(stderr, "read: unexpected EOF\n");
					return 2;
				}
			}

			strcpy(linkbuf, " => ");
			if((tmpint = fullread(archfd, linkbuf+4, fhead.size)) < fhead.size) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}
			linkbuf[tmpint + 3] = 0;

			tmpmode = S_IFLNK | fhead.perm;
			strmode(tmpmode, modebuf);
			break;
		  default:
			/* Skip data */
			if(skipaheadr(archfd, fhead.size)) {
				fprintf(stderr, "read: unexpected EOF\n");
				return 2;
			}

			/* Unknown type...Usually strmode shows '?' for us. */
			tmpmode = fhead.perm;
			strmode(tmpmode, modebuf);
		}
		tmptime = fhead.modify_time;
		strcpy(mtbuf, ctime(&tmptime));
		mtbuf[24] = 0;
		tmptime = fhead.create_time;
		strcpy(ztbuf, ctime(&tmptime));
		ztbuf[24] = 0;
		if(verbose)
			printf("%s %i (%s) %i (%s) %s %s %i %s%s\n",
				modebuf, (int)fhead.uid, fhead.username,
				(int)fhead.gid, fhead.groupname, mtbuf, ztbuf,
				(int)fhead.size, namebuf, linkbuf);
		else
			printf("%s\n", namebuf);
	}
}


void usage(char *name) {
	char *ptr;
	if(name && (ptr = strrchr(name, '/'))) name = ptr+1;

	fprintf(stderr,
"Benefits of using the far (\"File ARchive\") format over tar:
 - the format is simpler, smaller, more CPU efficient, and explicitly defined
 - support for a file creation time, for systems that support it
 - not based on outdated tape drive hardware
 - archives are more portable since there is only one version of the format
 - support for filenames and files of length up to 2^64-2 bytes
 - separate archives can be concatenated using the >> shell directive or cat,
and archive appending is pipe-friendly

For familiarity, most of the options for this utility reflect the options
provided by common tar utilities.

Usage: %s -{crtux}[ehilpUvVz] [-f archive_name] [-Z program] [file ...]
Actions: (Specify exactly one)
 -c	Create new archive
 -r -u	append to archive
 -t	show Table of contents (-v to list file properties)
 -x	eXtract archive
Options:
 -e	stop after an Error
 -f	Read/write archive from/to the specified File instead of stdin/stdout
 -h	Follow all symbolic links (for -c)
 -i	\"Insecure mode\": follow/show .. components of pathnames (for -x and -t)
 -l	Lock files while they are archived (for -c, -r, -u)
 -ll	wait for a Lock on files instead of skipping them (includes -l)
 -p	Preserve uid and gid; make stored permissions override umask (for -x)
	Also preserve SUID, SGID, and SVTX bits if ownerships are successfully
	preserved
 -U	Use text user/group names (for -xp), do not chown unavailable names
 -v	Verbose (lists files as they are added or extracted)
 -V	Show %% completion on each file (implies -v)
 -z	compress the archive using the default (de)compressor
	(currently "COMPRESS" and "UNCOMPRESS")
 -Z	specifies which (de)compressor to pipe the archive through (implies -z)
", name);
	exit(2);
}

extern char *optarg;
extern int optind;

int main(int argc, char **argv) {
	int archfd, tmpint, ret;

	while((tmpint = getopt(argc, argv, "cef:hilprtuUvVxzZ")) != -1) {
		switch(tmpint) {
		/* Actions */
		  case 'c':
			/* Create */
			action = 'c';
			break;
		  case 'r':
		  case 'u':
			/* Append */
			action = 'r';
			break;
		  case 't':
			/* List */
			action = 't';
			break;
		  case 'x':
			/* Extract */
			action = 'x';
			break;

		/* Options */
		  case 'e':
			/* Stop after error */
			stoperr = 1;
			break;
		  case 'f':
			/* Use file */
			if(!optarg)
				usage(argv[0]);
			if(!strcmp(optarg, "-"))
				archname = 0;
			else
				archname = optarg;
			break;
		  case 'h':
			/* Follow sym links */
			followsym = 1;
			break;
		  case 'i':
			/* Follow .. */
			insecure = 1;
			break;
		  case 'l':
			/* Lock files */
			uselocks++;
			break;
		  case 'p':
			/* Preserve perms */
			preserve = 1;
			break;
		  case 'U':
			/* Use stored text user/group names */
			usetext = 1;
			break;
		  case 'v':
			/* Verbose */
			verbose = 1;
			break;
		  case 'V':
			/* Show % completion */
			verbose = 1;
			completion = 1;
			break;
		  case 'z':
			/* Pipe through gzip/gunzip */
			gzip = 1;
			break;
		  case 'Z':
			/* Pipe through specified program */
			if(!optarg)
				usage(argv[0]);
			gzip = 1;
			compress = optarg;
			uncompress = optarg;
			break;
		  default:
			usage(argv[0]);
		}
	}
	if(!action) usage(argv[0]);
	argc -= optind;
	argv += optind;

	/* Open the archive, if necessary */
	if(archname) {
		switch(action) {
		  case 'c':
			tmpint = O_CREAT|O_WRONLY|O_APPEND|O_TRUNC;
			action = 'r';
			break;
		  case 'r':
			tmpint = O_WRONLY|O_APPEND;
			break;
		  case 't':
		  case 'x':
			tmpint = O_RDONLY;
			break;
		  default:
			fprintf(stderr, "Action %c on line %i\n", action, __LINE__);
			exit(100);
		}
		if((archfd = open(archname, tmpint, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH)) == -1) {
			fprintf(stderr, "%s: open: %s\n", archname, strerror(errno));
			return 2;
		}
	} else {
		/* Use stdout or stdin for the archive */
		switch(action) {
		  case 'c':
			action = 'r';
		  case 'r':
			archfd = STDOUT_FILENO;
			break;
		  case 't':
		  case 'x':
			archfd = STDIN_FILENO;
			break;
		  default:
			fprintf(stderr, "Action %c on line %i\n", action, __LINE__);
			exit(100);
		}
	}

	/* Pipe through gzip/gunzip if requested. */
	if(gzip) {
		int pp[2];
		pid_t child;

		if(pipe(pp)) {
			perror("pipe");
			return 2;
		}
		if((child = fork()) == -1) {
			perror("fork");
			return 2;
		}
		if(!child) {
			/* Child */

			if(action == 'r') {
				/* Child reads from the pipe during append/create */
				close(pp[1]);
				if(pp[0] != STDIN_FILENO) {
					if(dup2(pp[0], STDIN_FILENO) == -1)
						perror("dup2"), _exit(1);
					close(pp[0]);
				}
				if(archfd != STDOUT_FILENO) {
					if(dup2(archfd, STDOUT_FILENO) == -1)
						perror("dup2"), _exit(1);
					close(archfd);
				}
				execl(compress, compress, 0);
				perror(compress);
				_exit(1);
			} else {
				/* Child writes to the pipe during list/extract */
				close(pp[0]);
				if(pp[1] != STDOUT_FILENO) {
					if(dup2(pp[1], STDOUT_FILENO) == -1)
						perror("dup2"), _exit(1);
					close(pp[1]);
				}
				if(archfd != STDIN_FILENO) {
					if(dup2(archfd, STDIN_FILENO) == -1)
						perror("dup2"), _exit(1);
					close(archfd);
				}
				execl(uncompress, uncompress, 0);
				perror(uncompress);
				_exit(1);
			}
		}
		/* Parent */
		close(archfd);
		if(action == 'r') {
			/* Parent writes to the pipe during append/create */
			close(pp[0]);
			archfd = pp[1];
		} else {
			/* Parent reads from the pipe during list/extract */
			close(pp[1]);
			archfd = pp[0];
		}
	}

	/* Perform requested action */
	switch(action) {
	  case 'r':
		ret = append_things(archfd, argv);
		break;
	  case 't':
		ret = list_things(archfd, argv);
		break;
	  case 'x':
		ret = extract_things(archfd, argv);
		break;
	  default:
		fprintf(stderr, "Action %c on line %i\n", action, __LINE__);
		ret = 100;
	}

	close(archfd);
	return ret;
}

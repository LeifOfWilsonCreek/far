#ifndef _FAR_H
#define _FAR_H

#include <sys/types.h>
#include <sys/stat.h>

/*
The general format is:  for each file there is a header, a filename, and data,
like so:
[header][filename][data][header][filename][data]...
until EOF.

username: Null-termed ASCII username, max 31 characters plus null.  If not
available, use "".  Fill extra space with nulls.

groupname: Same format as username.

namesize: is the size of the filename, including the null.  Although the format
supports it, extracters may skip past files with names longer than 65536 (incl
null).

name: For convienence, the filename is already null-terminated and namesize
includes the terminator.  If ".." appears as a component of the path, it should
be stripped out, not followed.

Although the format supports links with data lengths longer than 65536 (incl
null), extracters may skip past them.
 */

struct far_header {
	u_int64_t magic;	/* Should equal FAR_MAGIC, indicates endian */
	u_int32_t type;		/* See below */
	u_int32_t perm;		/* See below */
	u_int64_t uid;		/* Numeric user id */
	u_int64_t gid;		/* Numeric group id */
	int64_t modify_time;	/* File's last mod time, seconds +/- epoch */
	int64_t create_time;	/* File's creation time, seconds +/- epoch */
	u_int64_t namesize;	/* Size of the file's name */
	u_int64_t size;		/* Size of the file's data */
	char username[32];	/* See below */
	char groupname[32];	/* See below */
};

#define FAR_MAGIC	((u_int64_t)0xfa70f1130f075470)

/* far_header.type can be one of the following: */
#define FAR_T_DELETED	0	/* deleted file (skip namesize+size) */
#define FAR_T_REGULAR	1	/* data section is a regular file */
#define FAR_T_DIRECTORY	2	/* zero-length data section */
#define FAR_T_LINK	3	/* data section stores the null-terminated
				   filename of another file in this archive */
#define FAR_T_SYMLINK	4	/* data section stores the null-terminated
				   value of a symbolic link */

/* far_header.perm is the bitwise OR of some of the following: */
#define FAR_P_SUID	04000
#define FAR_P_SGID	02000
#define FAR_P_SVTX	01000
#define FAR_P_RUSR	00400
#define FAR_P_WUSR	00200
#define FAR_P_XUSR	00100
#define FAR_P_RGRP	00040
#define FAR_P_WGRP	00020
#define FAR_P_XGRP	00010
#define FAR_P_ROTH	00004
#define FAR_P_WOTH	00002
#define FAR_P_XOTH	00001

/* Converts a 32 bit number from far/network/big endian to host endian */
#define HOST_2_FAR_32(a) FAR_2_HOST_32(a)

/* Converts a 32 bit number from host endian to far/network/big endian */
#define FAR_2_HOST_32(a) ( \
	((u_int32_t)(((unsigned char *)&(a))[0]) << 24) | \
	((u_int32_t)(((unsigned char *)&(a))[1]) << 16) | \
	((u_int32_t)(((unsigned char *)&(a))[2]) << 8 ) | \
	((u_int32_t)(((unsigned char *)&(a))[3]) << 0 ) )

/* Converts a 64 bit number from far/network/big endian to host endian */
#define HOST_2_FAR_64(a) FAR_2_HOST_64(a)

/* Converts a 64 bit number from host endian to far/network/big endian */
#define FAR_2_HOST_64(a) ( \
	((u_int64_t)(((unsigned char *)&(a))[0]) << 56) | \
	((u_int64_t)(((unsigned char *)&(a))[1]) << 48) | \
	((u_int64_t)(((unsigned char *)&(a))[2]) << 40) | \
	((u_int64_t)(((unsigned char *)&(a))[3]) << 32) | \
	((u_int64_t)(((unsigned char *)&(a))[4]) << 24) | \
	((u_int64_t)(((unsigned char *)&(a))[5]) << 16) | \
	((u_int64_t)(((unsigned char *)&(a))[6]) << 8 ) | \
	((u_int64_t)(((unsigned char *)&(a))[7]) << 0 ) )

/* Converts permission bits from host format to far format */
#define HOST_2_FAR_PERM(a) ( \
	((a) & S_IRUSR ? FAR_P_RUSR : 0) | \
	((a) & S_IWUSR ? FAR_P_WUSR : 0) | \
	((a) & S_IXUSR ? FAR_P_XUSR : 0) | \
	((a) & S_IRGRP ? FAR_P_RGRP : 0) | \
	((a) & S_IWGRP ? FAR_P_WGRP : 0) | \
	((a) & S_IXGRP ? FAR_P_XGRP : 0) | \
	((a) & S_IROTH ? FAR_P_ROTH : 0) | \
	((a) & S_IWOTH ? FAR_P_WOTH : 0) | \
	((a) & S_IXOTH ? FAR_P_XOTH : 0) | \
	((a) & S_ISUID ? FAR_P_SUID : 0) | \
	((a) & S_ISGID ? FAR_P_SGID : 0) | \
	((a) & S_ISVTX ? FAR_P_SVTX : 0) )

/* Converts permission bits from far format to host format */
#define FAR_2_HOST_PERM(a) ( \
	((a) & FAR_P_RUSR ? S_IRUSR : 0) | \
	((a) & FAR_P_WUSR ? S_IWUSR : 0) | \
	((a) & FAR_P_XUSR ? S_IXUSR : 0) | \
	((a) & FAR_P_RGRP ? S_IRGRP : 0) | \
	((a) & FAR_P_WGRP ? S_IWGRP : 0) | \
	((a) & FAR_P_XGRP ? S_IXGRP : 0) | \
	((a) & FAR_P_ROTH ? S_IROTH : 0) | \
	((a) & FAR_P_WOTH ? S_IWOTH : 0) | \
	((a) & FAR_P_XOTH ? S_IXOTH : 0) | \
	((a) & FAR_P_SUID ? S_ISUID : 0) | \
	((a) & FAR_P_SGID ? S_ISGID : 0) | \
	((a) & FAR_P_SVTX ? S_ISVTX : 0) )

/* Converts the entire header from host format to far format (normally you
 * would use this, not the individual macros above) */
#define HOST_2_FAR_HEADER(a)	\
	(a).magic =	HOST_2_FAR_64((a).magic);	\
	(a).type =	HOST_2_FAR_32((a).type);	\
	(a).perm =	HOST_2_FAR_PERM((a).perm);	\
	(a).perm =	HOST_2_FAR_32((a).perm);	\
	(a).uid =	HOST_2_FAR_64((a).uid);		\
	(a).gid =	HOST_2_FAR_64((a).gid);		\
	(a).modify_time = HOST_2_FAR_64((a).modify_time);	\
	(a).create_time = HOST_2_FAR_64((a).create_time);	\
	(a).namesize =	HOST_2_FAR_64((a).namesize);	\
	(a).size =	HOST_2_FAR_64((a).size);

/* Converts the entire header from far format to host format (normally you
 * would use this, not the individual macros above) */
#define FAR_2_HOST_HEADER(a)	\
	(a).magic =	FAR_2_HOST_64((a).magic);	\
	(a).type =	FAR_2_HOST_32((a).type);	\
	(a).perm =	FAR_2_HOST_32((a).perm);	\
	(a).perm =	FAR_2_HOST_PERM((a).perm);	\
	(a).uid =	FAR_2_HOST_64((a).uid);		\
	(a).gid =	FAR_2_HOST_64((a).gid);		\
	(a).modify_time = FAR_2_HOST_64((a).modify_time);	\
	(a).create_time = FAR_2_HOST_64((a).create_time);	\
	(a).namesize =	FAR_2_HOST_64((a).namesize);	\
	(a).size =	FAR_2_HOST_64((a).size);

#endif /* !_FAR_H */

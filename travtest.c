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

int main() {
	TRAV *tr;

	tr = trav_init("/", 0);//TRAV_DIRFIRST);
	if(!tr) {
		perror("tr");
		exit(1);
	}
	while(1) {
		trav_next(tr);
		if(tr->eof)
			break;
		if(tr->error)
			fprintf(stderr, "%s: %s\n", tr->name, strerror(tr->error));
		else
			printf("%s\n", tr->name);
	}
	if(tr->error)
		perror("Fatal error");
	trav_free(tr);
	return 0;
}

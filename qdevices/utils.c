/*
 * Copyright (c) 2015-2016 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Jan Friesse (jfriesse@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the Red Hat, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/file.h>
#include <arpa/inet.h>

#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include "utils.h"

/*
 * Check string to value on, off, yes, no, 0, 1. Return 1 if value is on, yes or 1, 0 if
 * value is off, no or 0 and -1 otherwise.
 */
int
utils_parse_bool_str(const char *str)
{

	if (strcasecmp(str, "yes") == 0 ||
	    strcasecmp(str, "on") == 0 ||
	    strcasecmp(str, "1") == 0) {
		return (1);
	} else if (strcasecmp(str, "no") == 0 ||
	    strcasecmp(str, "off") == 0 ||
	    strcasecmp(str, "0") == 0) {
		return (0);
	}

	return (-1);
}

int
utils_flock(const char *lockfile, pid_t pid,
    void (*log_printf)(int priority, const char *format, ...))
{
	struct flock lock;
	char pid_s[17];
	int fd_flag;
	int lf;
	int res;

	res = 0;

	lf = open(lockfile, O_WRONLY | O_CREAT, 0640);
	if (lf == -1) {
		log_printf(LOG_ERR, "Cannot create lock file. Error was %s", strerror(errno));

		return (-1);
	}

retry_fcntl:
	lock.l_type = F_WRLCK;
	lock.l_start = 0;
	lock.l_whence = SEEK_SET;
	lock.l_len = 0;
	if (fcntl(lf, F_SETLK, &lock) == -1) {
		switch (errno) {
		case EINTR:
			goto retry_fcntl;
			break;
		case EAGAIN:
		case EACCES:
			log_printf(LOG_ERR, "Another instance is already running.");
			res = -1;
			goto error_close;
			break;
		default:
			log_printf(LOG_ERR, "Cannot aquire lock. Error was %s", strerror(errno));
			res = -1;
			goto error_close;
			break;
		}
	}

	if (ftruncate(lf, 0) == -1) {
		log_printf(LOG_ERR, "Cannot truncate lock file. Error was %s", strerror(errno));
		res = -1;
		goto error_close_unlink;
	}

	memset(pid_s, 0, sizeof(pid_s));
	snprintf(pid_s, sizeof(pid_s) - 1, "%u\n", pid);

retry_write:
	if (write(lf, pid_s, strlen(pid_s)) != strlen(pid_s)) {
		if (errno == EINTR) {
			goto retry_write;
		} else {
			log_printf(LOG_ERR, "Cannot write pid to lock file. Error was %s",
			    strerror(errno));
			res = -1;
			goto error_close_unlink;
		}
	}

	if ((fd_flag = fcntl(lf, F_GETFD, 0)) == -1) {
		log_printf(LOG_ERR, "Cannot get close-on exec flag for lock file. Error was %s",
		    strerror(errno));
		res = -1;
		goto error_close_unlink;
	}
	fd_flag |= FD_CLOEXEC;
	if (fcntl(lf, F_SETFD, fd_flag) == -1) {
		log_printf(LOG_ERR, "Cannot set close-on-exec flag for lock file. Error was %s",
		    strerror(errno));
		res = -1;
		goto error_close_unlink;
	}

	return (res);

error_close_unlink:
	unlink(lockfile);
error_close:
	close(lf);

	return (res);
}

void
utils_tty_detach(void)
{
	int devnull;

	switch (fork()) {
		case -1:
			err(1, "Can't create child process");
			break;
		case 0:
			break;
		default:
			exit(0);
			break;
	}

	/* Create new session */
	(void)setsid();

	/*
	 * Map stdin/out/err to /dev/null.
	 */
	devnull = open("/dev/null", O_RDWR);
	if (devnull == -1) {
		err(1, "Can't open /dev/null");
	}

	if (dup2(devnull, 0) < 0 || dup2(devnull, 1) < 0
	    || dup2(devnull, 2) < 0) {
		close(devnull);
		err(1, "Can't dup2 stdin/out/err to /dev/null");
	}
	close(devnull);
}

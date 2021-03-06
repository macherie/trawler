/*
 * trawler.c
 *
 * Main routine for trawler.
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include "list.h"
#include "watcher.h"
#include "events.h"
#include "logging.h"

#define LOG_AREA "trawler"

pthread_cond_t exit_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t exit_mutex = PTHREAD_MUTEX_INITIALIZER;

int log_priority = LOG_ERR;
int use_syslog;
FILE *logfd;

static void *
signal_set(int signo, void (*func) (int))
{
	int r;
	struct sigaction sig;
	struct sigaction osig;

	sig.sa_handler = func;
	sigemptyset(&sig.sa_mask);
	sig.sa_flags = 0;

	r = sigaction(signo, &sig, &osig);

	if (r < 0)
		return (SIG_ERR);
	else
		return (osig.sa_handler);
}

static void sigend(int sig)
{
	pthread_mutex_lock(&exit_mutex);
	pthread_cond_signal(&exit_cond);
	pthread_mutex_unlock(&exit_mutex);
}

int trawl_dir(char *dirname)
{
	int num_files = 0;
	char fullpath[PATH_MAX];
	struct stat dirst;
	time_t dtime;
	DIR *dirfd;
	struct dirent *dirent;

	if (stat(dirname, &dirst) < 0) {
		err("Cannot open %s: error %d", dirname, errno);
		return 0;
	}
	if (difftime(dirst.st_atime, dirst.st_mtime) < 0) {
		dtime = dirst.st_atime;
	} else {
		dtime = dirst.st_mtime;
	}

	if (!S_ISDIR(dirst.st_mode)) {
#if 0
		if (insert_event(dirname, dtime) < 0)
			return 0;
		else
#endif
			return 1;
	}
	if (insert_inotify(dirname, 0) < 0)
		return 0;

	dirfd = opendir(dirname);
	if (!dirfd) {
		err("Cannot open directory %s: error %d", dirname, errno);
		return 0;
	}
	while ((dirent = readdir(dirfd))) {
		if (!strcmp(dirent->d_name, "."))
			continue;
		if (!strcmp(dirent->d_name, ".."))
			continue;
		if(snprintf(fullpath, PATH_MAX, "%s/%s",
			    dirname, dirent->d_name) >= PATH_MAX) {
			err("%s/%s: pathname overflow",
			    dirname, dirent->d_name);
		}
		if ((dirent->d_type & DT_REG) || (dirent->d_type & DT_DIR))
			num_files += trawl_dir(fullpath);
	}
	closedir(dirfd);
	return num_files;
}

unsigned long parse_time(char *optarg)
{
	struct tm c;
	char *p, *e;
	unsigned long val;
	double ret = 0;
	time_t now, test;

	now = test = time(NULL);
	if (localtime_r(&now, &c) == 0) {
		err("Cannot initialize time, error %d", errno);
		return 0;
	}
	p = optarg;
	while (p) {
		val = strtoul(p, &e, 10);
		if (p == e)
			break;
		dbg("%s %s %lu", p, e, val);
		if (!p) {
			ret = val;
			break;
		}
		switch (*e) {
		case 'Y':
			dbg("mon: %d %lu", c.tm_mon, val);
			c.tm_year += val;
			break;
		case 'M':
			dbg("mon: %d %lu", c.tm_mon, val);
			c.tm_mon += val;
			break;
		case 'D':
			dbg("day: %d %lu", c.tm_mday, val);
			c.tm_mday += val + 1;
			break;
		case 'h':
			dbg("hour: %d %lu", c.tm_hour, val);
			c.tm_hour += val;
			break;
		case 'm':
			dbg("min: %d %lu", c.tm_min, val);
			c.tm_min += val;
			break;
		case 's':
			dbg("sec: %d %lu", c.tm_sec, val);
			c.tm_sec += val;
			break;
		default:
			err("Invalid time specifier '%c'", *e);
			break;
		}
		p = e + 1;
	}
	test = mktime(&c);
	if (test == (time_t)-1) {
		err("Failed to convert time '%s'", optarg);
		ret = -1;
	}
	ret = difftime(test, now);
	info("Checking every %lu secs", (long)ret);

	return (long)ret;
}

int main(int argc, char **argv)
{
	int i, num_files;
	char init_dir[PATH_MAX];
	unsigned long checkinterval;
	time_t starttime, endtime;
	double elapsed;

	while ((i = getopt(argc, argv, "c:d:")) != -1) {
		switch (i) {
		case 'c':
			checkinterval = parse_time(optarg);
			if (checkinterval < 0) {
				err("Invalid time '%s'", optarg);
				return 1;
			}
			break;
		case 'd':
			realpath(optarg, init_dir);
			break;
		default:
			err("usage: %s [-d <dir>]", argv[0]);
			return 1;
		}
	}
	if (optind < argc) {
		err("usage: %s [-d <dir>]", argv[0]);
		return EINVAL;
	}
	if ('\0' == init_dir[0]) {
		strcpy(init_dir, "/");
	}
	if (!strcmp(init_dir, "..")) {
		if (chdir(init_dir) < 0) {
			err("Failed to change to parent directory: %d",
			    errno);
			return errno;
		}
		sprintf(init_dir, ".");
	}

	if (!strcmp(init_dir, ".") && !getcwd(init_dir, PATH_MAX)) {
		err("Failed to get current working directory");
		return errno;
	}

	signal_set(SIGINT, sigend);
	signal_set(SIGTERM, sigend);

	start_watcher();

	starttime = time(NULL);
	info("Starting at '%s'", init_dir);
	num_files = trawl_dir(init_dir);
	endtime = time(NULL);
	elapsed = difftime(endtime, starttime);
	info("Checked %d files in %f seconds", num_files, elapsed);

	list_events();

	pthread_cond_wait(&exit_cond, &exit_mutex);
	stop_watcher();

	return 0;
}

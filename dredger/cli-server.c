/*
 * cli-server.c
 *
 * Command line interface for dredger.
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/un.h>
#include <linux/netlink.h>
#include <pthread.h>

#include "logging.h"
#include "backend.h"
#include "dredger.h"
#include "migrate.h"
#include "cli.h"
#include "cli-server.h"

#define LOG_AREA "cli-server"

struct cli_monitor {
	int running;
	int sock;
	struct backend *be;
	int fanotify_fd;
	pthread_t thread;
};

void cli_monitor_cleanup(void *ctx)
{
	struct cli_monitor *cli = ctx;

	info("Shutdown cli monitor");
	if (cli->sock >= 0) {
		close(cli->sock);
		cli->sock = 0;
	}
	cli->thread = 0;
	cli->running = 0;
	free(cli);
}

void *cli_monitor_thread(void *ctx)
{
	struct cli_monitor *cli = ctx;

	cli->running = 1;
	pthread_cleanup_push(cli_monitor_cleanup, cli);

	while (cli->running) {
		int fdcount, ret, src_fd;
		uid_t src_uid;
		fd_set readfds;
		struct msghdr smsg;
		struct iovec iov;
		char cred_msg[CMSG_SPACE(sizeof(struct ucred)) + CMSG_SPACE(sizeof(int))];
		struct cmsghdr *cmsg;
		struct ucred *cred;
		enum cli_commands cli_cmd;
		char *filestr;
		static char buf[1024];
		struct sockaddr_un sun;
		socklen_t addrlen;
		size_t buflen;

		FD_ZERO(&readfds);
		FD_SET(cli->sock, &readfds);
		fdcount = select(cli->sock + 1, &readfds, NULL, NULL, NULL);
		if (fdcount < 0) {
			if (errno != EINTR)
				warn("error receiving message");
			continue;
		}
		memset(buf, 0x00, sizeof(buf));
		iov.iov_base = buf;
		iov.iov_len = 1024;
		memset(&sun, 0x00, sizeof(struct sockaddr_un));
		addrlen = sizeof(struct sockaddr_un);
		memset(&smsg, 0x00, sizeof(struct msghdr));
		smsg.msg_name = &sun;
		smsg.msg_namelen = addrlen;
		smsg.msg_iov = &iov;
		smsg.msg_iovlen = 1;
		smsg.msg_control = cred_msg;
		smsg.msg_controllen = sizeof(cred_msg);

		buflen = recvmsg(cli->sock, &smsg, 0);
		if (buflen < 0) {
			if (errno != EINTR)
				err("error receiving cli message, errno %d",
				    errno);
			continue;
		}
		src_fd = -1;
		src_uid = -1;
		for (cmsg = CMSG_FIRSTHDR(&smsg); cmsg != NULL;
		     cmsg = CMSG_NXTHDR(&smsg, cmsg)) {
			if (cmsg->cmsg_level != SOL_SOCKET)
				continue;
			switch (cmsg->cmsg_type) {
			case SCM_CREDENTIALS:
				cred = (struct ucred *)CMSG_DATA(cmsg);
				src_uid = cred->uid;
				break;
			case SCM_RIGHTS:
				src_fd = *(int *)CMSG_DATA(cmsg);
				break;
			}
		}

		if (src_uid != 0) {
			warn("Invalid message (uid=%d, fd %d), ignoring",
			     src_uid, src_fd);
			continue;
		}
		info("received %d/%d bytes from %s", buflen, sizeof(buf),
		     &sun.sun_path[1]);
		cli_cmd = buf[0];
		filestr = buf + 1;
		if (cli_cmd != CLI_SHUTDOWN && !strlen(filestr)) {
			info("%s: skipping event '%d', no file specified",
			     cli_cmd);
			cli_cmd = CLI_NOFILE;
		}
		info("CLI event '%d' fd %d file '%s'", cli_cmd,
		     src_fd, filestr);

		switch (cli_cmd) {
		case CLI_NOFILE:
			ret = EINVAL;
			break;
		case CLI_SHUTDOWN:
			pthread_kill(daemon_thr, SIGTERM);
			cli->running = 0;
			ret = 0;
			break;
		case CLI_MIGRATE:
			ret = migrate_file(cli->be, src_fd, filestr);
			break;
		case CLI_CHECK:
			ret = check_backend(cli->be, filestr);
			if (ret < 0) {
				err("File '%s' could not be checked, error %d",
				    filestr, -ret);
				ret = -ret;
			} else if (ret > 0) {
				info("File '%s' needs migration", filestr);
				ret = 0;
			} else {
				info("File '%s' up-to-date", filestr);
				ret = EALREADY;
			}
			break;
		case CLI_MONITOR:
			ret = monitor_file(cli->fanotify_fd, filestr);
			break;
		case CLI_SETUP:
			ret = migrate_file(cli->be, -1, filestr);
			break;
		default:
			info("%s: Unhandled event %d",filestr, cli_cmd);
			ret = EINVAL;
			break;
		}
		if (ret) {
			buf[0] = ret;
			iov.iov_len = 1;
		} else {
			buf[0] = 0;
			iov.iov_len = 0;
		}

		if (sendmsg(cli->sock, &smsg, 0) < 0)
			err("sendmsg failed, error %d", errno);
		if (src_fd >= 0)
			close(src_fd);
	}
	pthread_cleanup_pop(1);
	return ((void *)0);
}

pthread_t start_cli(struct backend *be, int fanotify_fd)
{
	struct cli_monitor *cli;
	struct sockaddr_un sun;
	socklen_t addrlen;
	const int feature_on = 1;
	int rc = 0;

	info("Start cli monitor");

	cli = malloc(sizeof(struct cli_monitor));
	memset(cli, 0, sizeof(struct cli_monitor));
	cli->fanotify_fd = fanotify_fd;
	cli->be = be;

	memset(&sun, 0x00, sizeof(struct sockaddr_un));
	sun.sun_family = AF_LOCAL;
	strcpy(&sun.sun_path[1], "/org/kernel/trawler/dredger");
	addrlen = offsetof(struct sockaddr_un, sun_path) +
		strlen(sun.sun_path + 1) + 1;
	cli->sock = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (cli->sock < 0) {
		err("cannot open cli socket, error %d", errno);
		free(cli);
		return (pthread_t)0;
	}
	if (bind(cli->sock, (struct sockaddr *) &sun, addrlen) < 0) {
		err("cannot bind cli socket, error %d", errno);
		close(cli->sock);
		free(cli);
		return (pthread_t)0;
	}
	setsockopt(cli->sock, SOL_SOCKET, SO_PASSCRED,
		   &feature_on, sizeof(feature_on));

	rc = pthread_create(&cli->thread, NULL, cli_monitor_thread, cli);
	if (rc) {
		cli->thread = 0;
		close(cli->sock);
		err("Failed to start cli monitor: %d", errno);
		free(cli);
		return (pthread_t)0;
	}

	return cli->thread;
}

void stop_cli(pthread_t cli_thr)
{
	pthread_cancel(cli_thr);
	pthread_join(cli_thr, NULL);
}


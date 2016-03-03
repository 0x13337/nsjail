/*

   nsjail - isolating the binary
   -----------------------------------------

   Copyright 2014 Google Inc. All Rights Reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

#include "contain.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <linux/capability.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#include "log.h"
#include "mount.h"
#include "net.h"
#include "util.h"

bool containInitNetNs(struct nsjconf_t * nsjconf)
{
	return netInitNs(nsjconf);
}

static bool containSetGroups(pid_t pid)
{
	/*
	 * No need to write 'deny' to /proc/pid/setgroups if our euid==0, as writing to uid_map/gid_map
	 * will succeed anyway
	 */
	if (geteuid() == 0) {
		return true;
	}

	char fname[PATH_MAX];
	snprintf(fname, sizeof(fname), "/proc/%d/setgroups", pid);
	const char *denystr = "deny";
	if (utilWriteBufToFile(fname, denystr, strlen(denystr), O_WRONLY) == false) {
		LOG_E("utilWriteBufToFile('%s', '%s') failed", fname, denystr);
		return false;
	}
	return true;
}

static bool containUidGidMap(struct nsjconf_t *nsjconf, pid_t pid)
{
	if (nsjconf->clone_newuser == false) {
		return true;
	}

	char fname[PATH_MAX];
	char map[128];

	snprintf(fname, sizeof(fname), "/proc/%d/uid_map", pid);
	snprintf(map, sizeof(map), "%lu %lu 1", (unsigned long)nsjconf->inside_uid,
		 (unsigned long)nsjconf->outside_uid);
	LOG_D("Writing '%s' to '%s'", map, fname);
	if (utilWriteBufToFile(fname, map, strlen(map), O_WRONLY) == false) {
		LOG_E("utilWriteBufToFile('%s', '%s') failed", fname, map);
		return false;
	}

	snprintf(fname, sizeof(fname), "/proc/%d/gid_map", pid);
	snprintf(map, sizeof(map), "%lu %lu 1", (unsigned long)nsjconf->inside_gid,
		 (unsigned long)nsjconf->outside_gid);
	LOG_D("Writing '%s' to '%s'", map, fname);
	if (utilWriteBufToFile(fname, map, strlen(map), O_WRONLY) == false) {
		LOG_E("utilWriteBufToFile('%s', '%s') failed", fname, map);
		return false;
	}
	return true;
}

bool containInitUserNs(struct nsjconf_t * nsjconf, pid_t pid)
{
	if (containSetGroups(pid) == false) {
		return false;
	}
	if (containUidGidMap(nsjconf, pid) == false) {
		return false;
	}
	return true;
}

bool containDropPrivs(struct nsjconf_t * nsjconf)
{
	/*
	 * Best effort because of /proc/self/setgroups
	 */
	gid_t *group_list = NULL;
	if (setgroups(0, group_list) == -1) {
		PLOG_D("setgroups(NULL) failed");
	}
	if (setresgid(nsjconf->inside_gid, nsjconf->inside_gid, nsjconf->inside_gid) == -1) {
		PLOG_E("setresgid(%u)", nsjconf->inside_gid);
		return false;
	}
	if (setresuid(nsjconf->inside_uid, nsjconf->inside_uid, nsjconf->inside_uid) == -1) {
		PLOG_E("setresuid(%u)", nsjconf->inside_uid);
		return false;
	}
#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1) {
		/* Only new kernels support it */
		PLOG_W("prctl(PR_SET_NO_NEW_PRIVS, 1)");
	}

	if (nsjconf->keep_caps == false) {
		if (prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) == -1) {
			PLOG_E("prctl(PR_SET_KEEPCAPS, 0)");
			return false;
		}
		struct __user_cap_header_struct cap_hdr = {
			.version = _LINUX_CAPABILITY_VERSION_3,
			.pid = 0,
		};
		const struct __user_cap_data_struct cap_data[_LINUX_CAPABILITY_U32S_3] = {
			[0 ... (_LINUX_CAPABILITY_U32S_3 - 1)].inheritable = 0U,
			[0 ... (_LINUX_CAPABILITY_U32S_3 - 1)].effective = 0U,
			[0 ... (_LINUX_CAPABILITY_U32S_3 - 1)].permitted = 0U,
		};
		if (syscall(__NR_capset, &cap_hdr, &cap_data) == -1) {
			PLOG_E("capset()");
			return false;
		}
	}
	return true;
}

bool containPrepareEnv(struct nsjconf_t * nsjconf)
{
	LOG_D("Setting hostname to '%s'", nsjconf->hostname);
	if (nsjconf->clone_newuts) {
		if (sethostname(nsjconf->hostname, strlen(nsjconf->hostname)) == -1) {
			PLOG_E("sethostname('%s')", nsjconf->hostname);
			return false;
		}
	}
	if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) == -1) {
		PLOG_E("prctl(PR_SET_PDEATHSIG, SIGKILL)");
		return false;
	}
	if (nsjconf->personality && personality(nsjconf->personality) == -1) {
		PLOG_E("personality(%lx)", nsjconf->personality);
		return false;
	}
	errno = 0;
	if (setpriority(PRIO_PROCESS, 0, 19) == -1 && errno != 0) {
		PLOG_W("setpriority(19)");
	}
	if (nsjconf->skip_setsid == false) {
		setsid();
	}
	return true;
}

bool containInitMountNs(struct nsjconf_t * nsjconf)
{
	return mountInitNs(nsjconf);
}

bool containSetLimits(struct nsjconf_t * nsjconf)
{
	struct rlimit64 rl;
	rl.rlim_cur = rl.rlim_max = nsjconf->rl_as;
	if (prlimit64(0, RLIMIT_AS, &rl, NULL) == -1) {
		PLOG_E("prlimit64(0, RLIMIT_AS, %" PRIu64 ")", nsjconf->rl_as);
		return false;
	}
	rl.rlim_cur = rl.rlim_max = nsjconf->rl_core;
	if (prlimit64(0, RLIMIT_CORE, &rl, NULL) == -1) {
		PLOG_E("prlimit64(0, RLIMIT_CORE, %" PRIu64 ")", nsjconf->rl_core);
		return false;
	}
	rl.rlim_cur = rl.rlim_max = nsjconf->rl_cpu;
	if (prlimit64(0, RLIMIT_CPU, &rl, NULL) == -1) {
		PLOG_E("prlimit64(0, RLIMIT_CPU, %" PRIu64 ")", nsjconf->rl_cpu);
		return false;
	}
	rl.rlim_cur = rl.rlim_max = nsjconf->rl_fsize;
	if (prlimit64(0, RLIMIT_FSIZE, &rl, NULL) == -1) {
		PLOG_E("prlimit64(0, RLIMIT_FSIZE, %" PRIu64 ")", nsjconf->rl_fsize);
		return false;
	}
	rl.rlim_cur = rl.rlim_max = nsjconf->rl_nofile;
	if (prlimit64(0, RLIMIT_NOFILE, &rl, NULL) == -1) {
		PLOG_E("prlimit64(0, RLIMIT_NOFILE, %" PRIu64 ")", nsjconf->rl_nofile);
		return false;
	}
	rl.rlim_cur = rl.rlim_max = nsjconf->rl_nproc;
	if (prlimit64(0, RLIMIT_NPROC, &rl, NULL) == -1) {
		PLOG_E("prlimit64(0, RLIMIT_NPROC, %" PRIu64 ")", nsjconf->rl_nproc);
		return false;
	}
	rl.rlim_cur = rl.rlim_max = nsjconf->rl_stack;
	if (prlimit64(0, RLIMIT_STACK, &rl, NULL) == -1) {
		PLOG_E("prlimit64(0, RLIMIT_STACK, %" PRIu64 ")", nsjconf->rl_stack);
		return false;
	}
	return true;
}

static bool containMakeFdsCOENaive(void)
{
	// Don't use getrlimit(RLIMIT_NOFILE) here, as it can return an artifically small value
	// (e.g. 32), which could be smaller than a maximum assigned number to file-descriptors
	// in this process. Just use some reasonably sane value (e.g. 1024)
	for (unsigned fd = (STDERR_FILENO + 1); fd < 1024; fd++) {
		int flags = fcntl(fd, F_GETFD, 0);
		if (flags == -1) {
			continue;
		}
		fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
		LOG_D("Set fd '%d' flag to FD_CLOEXEC", fd);
	}
	return true;
}

static bool containMakeFdsCOEProc(void)
{
	/* Make all fds above stderr close-on-exec */
	DIR *dir = opendir("/proc/self/fd");
	if (dir == NULL) {
		PLOG_D("opendir('/proc/self/fd')");
		return false;
	}
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (entry == NULL && errno != 0) {
			PLOG_D("readdir('/proc/self/fd')");
			closedir(dir);
			return false;
		}
		if (entry == NULL) {
			break;
		}
		if (strcmp(".", entry->d_name) == 0) {
			continue;
		}
		if (strcmp("..", entry->d_name) == 0) {
			continue;
		}
		int fd = strtoul(entry->d_name, NULL, 10);
		if (errno == EINVAL) {
			LOG_W("Cannot convert /proc/self/fd/%s to a number", entry->d_name);
			continue;
		}
		if (fd > STDERR_FILENO) {
			int flags = fcntl(fd, F_GETFD, 0);
			if (flags == -1) {
				PLOG_D("fcntl(fd, F_GETFD, 0)");
				return false;
			}
			fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
			LOG_D("Set fd '%d' flag to FD_CLOEXEC", fd);
		}
	}
	closedir(dir);
	return true;
}

bool containMakeFdsCOE(void)
{
	if (containMakeFdsCOEProc() == true) {
		return true;
	}
	if (containMakeFdsCOENaive() == true) {
		return true;
	}
	LOG_E("Couldn't mark relevant file-descriptors as close-on-exec with any known method");
	return false;
}

bool containSetupFD(struct nsjconf_t * nsjconf, int fd_in, int fd_out, int fd_err, int fd_log)
{
	/* Make sure all logs go to the parent process from now on */
	if (fd_log != -1) {
		logRedirectLogFD(fd_log);
	}

	if (nsjconf->mode != MODE_LISTEN_TCP) {
		if (nsjconf->is_silent == false) {
			return true;
		}
		if ((fd_in = fd_out = fd_err = open("/dev/null", O_RDWR)) == -1) {
			PLOG_E("open('/dev/null', O_RDWR)");
			return false;
		}
	}
	/* Set stdin/stdout/stderr to the net */
	if (dup2(fd_in, STDIN_FILENO) == -1) {
		PLOG_E("dup2(%d, STDIN_FILENO)", fd_in);
		return false;
	}
	if (dup2(fd_out, STDOUT_FILENO) == -1) {
		PLOG_E("dup2(%d, STDOUT_FILENO)", fd_out);
		return false;
	}
	if (dup2(fd_err, STDERR_FILENO) == -1) {
		PLOG_E("dup2(%d, STDERR_FILENO)", fd_err);
		return false;
	}
	return true;
}

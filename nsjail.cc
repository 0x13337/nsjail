/*

   nsjail
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

#include "nsjail.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "cmdline.h"
#include "common.h"
#include "log.h"
#include "net.h"
#include "subproc.h"
#include "util.h"

static __thread int nsjailSigFatal = 0;
static __thread bool nsjailShowProc = false;

static void nsjailSig(int sig) {
	if (sig == SIGALRM) {
		return;
	}
	if (sig == SIGCHLD) {
		return;
	}
	if (sig == SIGUSR1 || sig == SIGQUIT) {
		nsjailShowProc = true;
		return;
	}
	nsjailSigFatal = sig;
}

static bool nsjailSetSigHandler(int sig) {
	LOG_D("Setting sighandler for signal %s (%d)", utilSigName(sig), sig);

	sigset_t smask;
	sigemptyset(&smask);

	struct sigaction sa;
	sa.sa_handler = nsjailSig;
	sa.sa_mask = smask;
	sa.sa_flags = 0;
	sa.sa_restorer = NULL;
	if (sigaction(sig, &sa, NULL) == -1) {
		PLOG_E("sigaction(%d)", sig);
		return false;
	}
	return true;
}

static bool nsjailSetSigHandlers(void) {
	for (size_t i = 0; i < ARRAYSIZE(nssigs); i++) {
		if (!nsjailSetSigHandler(nssigs[i])) {
			return false;
		}
	}
	return true;
}

static bool nsjailSetTimer(struct nsjconf_t* nsjconf) {
	if (nsjconf->mode == MODE_STANDALONE_EXECVE) {
		return true;
	}

	struct itimerval it = {
	    .it_interval =
		{
		    .tv_sec = 1,
		    .tv_usec = 0,
		},
	    .it_value =
		{
		    .tv_sec = 1,
		    .tv_usec = 0,
		},
	};
	if (setitimer(ITIMER_REAL, &it, NULL) == -1) {
		PLOG_E("setitimer(ITIMER_REAL)");
		return false;
	}
	return true;
}

static void nsjailListenMode(struct nsjconf_t* nsjconf) {
	int listenfd = netGetRecvSocket(nsjconf->bindhost, nsjconf->port);
	if (listenfd == -1) {
		return;
	}
	for (;;) {
		if (nsjailSigFatal > 0) {
			subproc::killAll(nsjconf);
			logStop(nsjailSigFatal);
			close(listenfd);
			return;
		}
		if (nsjailShowProc) {
			nsjailShowProc = false;
			subproc::displayProc(nsjconf);
		}
		int connfd = netAcceptConn(listenfd);
		if (connfd >= 0) {
			subproc::runChild(nsjconf, connfd, connfd, connfd);
			close(connfd);
		}
		subproc::reapProc(nsjconf);
	}
}

static int nsjailStandaloneMode(struct nsjconf_t* nsjconf) {
	subproc::runChild(nsjconf, STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
	for (;;) {
		int child_status = subproc::reapProc(nsjconf);

		if (subproc::countProc(nsjconf) == 0) {
			if (nsjconf->mode == MODE_STANDALONE_ONCE) {
				return child_status;
			}
			subproc::runChild(nsjconf, STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
			continue;
		}
		if (nsjailShowProc) {
			nsjailShowProc = false;
			subproc::displayProc(nsjconf);
		}
		if (nsjailSigFatal > 0) {
			subproc::killAll(nsjconf);
			logStop(nsjailSigFatal);
			return -1;
		}

		pause();
	}
	// not reached
}

int main(int argc, char* argv[]) {
	std::unique_ptr<struct nsjconf_t> nsjconf = cmdline::parseArgs(argc, argv);
	if (!nsjconf) {
		LOG_F("Couldn't parse cmdline options");
	}
	if (nsjconf->clone_newuser == false && geteuid() != 0) {
		LOG_W("--disable_clone_newuser might require root() privs");
	}
	if (nsjconf->daemonize && (daemon(0, 0) == -1)) {
		PLOG_F("daemon");
	}
	cmdline::logParams(nsjconf.get());
	if (nsjailSetSigHandlers() == false) {
		LOG_F("nsjailSetSigHandlers() failed");
	}
	if (nsjailSetTimer(nsjconf.get()) == false) {
		LOG_F("nsjailSetTimer() failed");
	}

	if (nsjconf->mode == MODE_LISTEN_TCP) {
		nsjailListenMode(nsjconf.get());
	} else {
		return nsjailStandaloneMode(nsjconf.get());
	}
	return 0;
}

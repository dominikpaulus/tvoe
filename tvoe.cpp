#include <stdio.h>
#include <stdlib.h>
#include <event.h>
#include <event2/thread.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include "http.h"
#include "log.h"
#include "tvoe.h"

struct event_base *evbase;
const char *conffile = "./tvoe.conf";
extern int loglevel;
static bool daemonize = true;
bool daemonized = false;
int http_port = 8080;

extern void yylex_destroy();
extern void init_lexer();
extern void init_parser();
extern int yyparse(void);

int main(int argc, char **argv) {
	int c;
	char *pidfile = NULL;
	bool quiet = false;

	while((c = getopt(argc, argv, "qhfd:c:p:")) != -1) {
		switch(c) {
			case 'c': // Config filename
				conffile = optarg;
				break;
			case 'f': // Foreground
				daemonize = false;
				break;
			case 'p': // Pidfile
				pidfile = optarg;
				break;
			case 'q': // Quiet
				quiet = true;
				break;
			case 'h':
			default:
				fprintf(stderr, "Usage: %s [-c config] [-f] [-h] [-p pidfile]\n"
						"\t-c: Sets configuration file path. Default: ./tvoe.conf\n"
						"\t-f: Disable daemon fork\n"
						"\t-p: Write PID to given pidfile\n"
						"\t-q: Quiet startup\n"
						"\t-h: Show this help\n", argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	if(!quiet)
		printf("tvoe version %s compiled on %s %s\n", "0.1", __DATE__, __TIME__);

	/* Initialize libevent */
	evthread_use_pthreads();
	event_init();

	evbase = event_base_new();
	if(!evbase) {
		logger(LOG_ERR, "Unable to create event base!");
		exit(EXIT_FAILURE);
	}

	/* Parse config file */
	init_lexer();
	init_parser();
	yyparse();
	yylex_destroy();

	/* Initialize logging subsystem */
	init_log();

	/* Open HTTP listener */
	if(http_init(http_port) < 0) {
		logger(LOG_ERR, "Unable to open HTTP listener, aborting.");
		return EXIT_FAILURE;
	}

	if(!quiet)
		logger(LOG_INFO, "tvoe starting");

	// Daemonize if necessary
	if(daemonize) {
		if(!quiet)
			printf("Daemonizing... ");
		fflush(stdout);
		pid_t pid = fork();
		if(pid < 0) {
			perror("fork()");
			return EXIT_FAILURE;
		}
		if(pid > 0) {// Exit parent
			if(!quiet)
				printf("success. (pid: %u)\n", pid);
			return EXIT_SUCCESS;
		}

		if(pidfile) {
			int pidfd = open(pidfile, O_RDWR | O_CREAT, 0600);
			char pid[8]; // PID is not greater than 65536
			if(pidfd < 0) {
				logger(LOG_ERR, "Unable to open PID file %s: %s, exiting",
						pidfile, strerror(errno));
				return EXIT_FAILURE;
			}
			if(lockf(pidfd, F_TLOCK, 0) < 0) {
				logger(LOG_ERR, "Unable to lock PID file %s. tvoe is probably already running.",
						pidfile);
				return EXIT_FAILURE;
			}
			snprintf(pid, 8, "%d\n", getpid());
			if(write(pidfd, pid, strlen(pid)) != (ssize_t) strlen(pid)) {
				logger(LOG_ERR, "Unable to write to PID file %s: %s",
						pidfile, pidfile);
				return EXIT_FAILURE;
			}
		}

		daemonized = true; // prevents logger from logging to stderr
		umask(0);
		if(setsid() < 0) {
			perror("setsid()");
			exit(EXIT_FAILURE);
		}
		if(chdir("/") < 0) {
			perror("chdir()");
			exit(EXIT_FAILURE);
		}
		freopen("/dev/null", "r", stdin);
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
	}

	/* Initialize frontend handler */
	frontend_init();

	/* Ignore SIGPIPE */
	{
		struct sigaction action;
		sigemptyset(&action.sa_mask);
		action.sa_flags = SA_RESTART;
		action.sa_handler = SIG_IGN;
		sigaction(SIGPIPE, &action, NULL);
	}

	event_base_dispatch(evbase);

	logger(LOG_ERR, "Event loop exited");

	return EXIT_SUCCESS;
}

#include <stdio.h>
#include <stdlib.h>
#include <event.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include "http.h"
#include "log.h"

const char *conffile = "./getstream.conf";
extern int loglevel;
bool daemonize = true;

extern void yylex_destroy();
extern void init_lexer();
extern void init_parser();
extern int yyparse(void);

int main(int argc, char **argv) {
	int c;

	printf("getstream-sh version %s compiled on %s %s\n", "0.1", __DATE__, __TIME__);

	while((c = getopt(argc, argv, "hfd:c:")) != -1) {
		switch(c) {
			case 'c': // Config filename
				conffile = optarg;
				break;
			case 'f': // Foreground
				daemonize = false;
				break;
			case 'h':
			default:
				fprintf(stderr, "Usage: %s [-c config] [-f] [-h]\n"
						"\t-c: Sets configuration file path. Default: ./getstream.conf\n"
						"\t-f: Disable daemon fork\n"
						"\t-h: Show this help\n", argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	/* Initialize libevent */
	event_init();
	httpd = evhttp_new(NULL);

	/* Parse config file */
	init_lexer();
	init_parser();
	yyparse();
	yylex_destroy();

	/* Initialize logging subsystem */
	init_log();

	// Daemonize if necessary
	if(daemonize && getppid() != 1) {
		printf("Daemonizing... ");
		fflush(stdout);
		pid_t pid = fork();
		if(pid < 0) {
			perror("fork()");
			return EXIT_FAILURE;
		}
		if(pid > 0) {// Exit parent
			printf("success. (pid: %u)\n", pid);
			return EXIT_SUCCESS;
		}
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

	/* Ignore SIGPIPE */
	{
		struct sigaction action;
		sigemptyset(&action.sa_mask);
		action.sa_flags = SA_RESTART;
		action.sa_handler = SIG_IGN;
		sigaction(SIGPIPE, &action, NULL);
	}

	event_dispatch();

	logger(LOG_ERR, "Event loop exited");

	return EXIT_SUCCESS;
}

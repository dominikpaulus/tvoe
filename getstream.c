#include <stdio.h>
#include <stdlib.h>
#include <event.h>
#include <unistd.h>
#include <stdbool.h>
#include "http.h"

const char *conffile = "./getstream.conf";
int loglevel = 1;

int main(int argc, char **argv) {
	int c;
	bool daemonize = true;

	printf("getstream-sh version %s compiled on %s %s\n", "0.1", __DATE__, __TIME__);

	while((c = getopt(argc, argv, "hfd:c:")) != -1) {
		switch(c) {
			case 'd': // Debug level
				loglevel = atoi(optarg);
				break;
			case 'c': // Config filename
				conffile = optarg;
				break;
			case 'f':
				daemonize = false;
				break;
			case 'h':
			default:
				fprintf(stderr, "Usage: %s [-c config] [-d loglevel] [-f] [-h]\n"
						"\t-c: Sets configuration file path. Default: ./getstream.conf\n"
						"\t-d: Set debug level (1-10)\n"
						"\t-f: Disable daemon fork\n"
						"\t-h: Show this help\n", argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	event_init();
	httpd = evhttp_new(NULL);

	init_lexer();
	init_parser();
	yyparse();
	yylex_destroy();

	// Daemonize if necessary
	if(daemonize && getppid() != 1) {
		pid_t pid = fork();
		if(pid < 0) {
			perror("fork()");
			return EXIT_FAILURE;
		}
		if(pid > 0)	// Exit parent
			return EXIT_SUCCESS;
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

	event_dispatch();
}

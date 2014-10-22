#include <printf.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>

char *logfile = NULL;
int use_syslog = 0;
int loglevel = 1;
static FILE * log_fd;
extern bool daemonized;

void logger(int level, char *fmt, ...) {
	char text[2048], tv[256];
	time_t t;
	struct tm * ti;

	// TODO: Filter loglevel

	time(&t);
	ti = localtime(&t);
	strftime(tv, sizeof(tv), "[%X %x]", ti);

	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);

	if(log_fd) {
		fprintf(log_fd, "%s %s\n", tv, text);
		fflush(log_fd);
	}
	if(use_syslog)
		syslog(level, "%s", text);
	if(!daemonized)
		fprintf(stderr, "%s %s\n", tv, text);
}

int init_log(void) {
	if(logfile) {
		log_fd = fopen(logfile, "a");
		if(!log_fd)
			fprintf(stderr, "Unable to open logfile %s: %s\n", logfile,
					strerror(errno));
	}
	if(use_syslog)
		openlog("tvoe", 0, LOG_DAEMON);
	logger(LOG_INFO, "tvoe starting");
	return 0;
}

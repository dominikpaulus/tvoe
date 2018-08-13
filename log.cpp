#include <printf.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

char *logfile = NULL;
int use_syslog = 0;
int loglevel = 2;
static FILE * log_fd;
extern bool daemonized;

void logger(int level, const char *fmt, ...) {
	char text[2048], timestamp[256];
	time_t t;
	struct tm * ti;

	if((loglevel == 0) ||
	   (loglevel == 1 && (level != LOG_ERR)) ||
	   (loglevel == 2 && (level != LOG_ERR && level != LOG_NOTICE)) ||
	   (loglevel == 3 && (level == LOG_DEBUG)))
		return;

	time(&t);
	ti = localtime(&t);
	strftime(timestamp, sizeof(timestamp), "[%X %x]", ti);

	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);

	if(log_fd) {
		fprintf(log_fd, "%s %s\n", timestamp, text);
		fflush(log_fd);
	}
	if(use_syslog)
		syslog(level, "%s", text);
	if(!daemonized) {
		FILE *out;
		if(level == LOG_ERR || level == LOG_NOTICE)
			out = stderr;
		else
			out = stdout;

		/*
		 * Do not log timestamps if output is not interactive
		 * (e.g. if we are running under systemd-supervision and
		 * output is sent to syslog anyway)
		 */
		if(isatty(STDOUT_FILENO))
			fprintf(out, "%s %s\n", timestamp, text);
		else
			fprintf(out, "%s\n", text);
	}
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
	return 0;
}

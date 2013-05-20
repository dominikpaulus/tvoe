#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <glib.h>
#include <stdlib.h>
#include "channels.h"
#include "log.h"
#include "http.h"
#include "mpeg.h"

#define MAXTOKS 2048

struct tokens {
	char *t[MAXTOKS];
	int count;
};

static struct tokens tokenize(char *str) {
	int i = 0;
	struct tokens ret;
	ret.count = 0;
	ret.t[0] = strtok(str, ":");
	if(ret.t[0] == NULL)
		return ret;
	for(i=1; i < MAXTOKS && (ret.t[i] = strtok(NULL, ":")); i++)
		;
	ret.count = i;
	return ret;
}

int parse_channels(const char *file) {
	int i;
	FILE * fd = fopen(file, "r");
	if(!fd) {
		logger(LOG_ERR, "channels.conf open failed: %s",
				strerror(errno));
		return -1;
	}
	char buf[2048];
	for(i=1; fgets(buf, sizeof(buf), fd); i++) {
		if(feof(fd) || ferror(fd))
			break;
		struct tokens tok = tokenize(buf);
		if(tok.count != 9) {
			logger(LOG_ERR, "Parsing channel config failed: Line %d: "
					"Invalid number of tokens (was: %d, expected: %d)",
					i, tok.count, 9);
			continue;
		}
		/*if(atoi(tok.t[7]) >= MAX_PID) {
			logger(LOG_ERR, "Parsing channels config failed: Line %d: "
					"Invalid SID: %s", i, tok.t[7]);
			continue;
		}*/
		bool pol = tok.t[2][0] == 'h';
		struct tune l = { 0, { // unused
			atoi(tok.t[8]), // delivery system
			atoi(tok.t[1]) * 1000, // frequency
			atoi(tok.t[4]) * 1000, // symbol rate
			pol }, atoi(tok.t[7]) }; // polarization and SID
		add_channel(tok.t[0], atoi(tok.t[7]), l);
	}
	if(ferror(fd)) {
		logger(LOG_ERR, "channels.conf read failed: %s",
				strerror(errno));
		return -1;
	}

	return 0;
}

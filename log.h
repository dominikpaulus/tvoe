#ifndef __INCLUDED_GETSH_LOGGER
#define __INCLUDED_GETSH_LOGGER
#include <syslog.h>

extern int loglevel;

void logger(int loglevel, const char *fmt, ...);
int init_log(void);

#endif

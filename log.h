#ifndef __INCLUDED_TVOE_LOGGER
#define __INCLUDED_TVOE_LOGGER
#include <syslog.h>

extern int loglevel;

void logger(int loglevel, const char *fmt, ...);
int init_log(void);

#endif

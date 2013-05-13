#ifndef __INCLUDED_GETSH_LOGGER
#define __INCLUDED_GETSH_LOGGER

#define LOG_EMERG  0
#define LOG_CRIT   1
#define LOG_NOTICE 2
#define LOG_INFO   3
#define LOG_DEBUG  4

void logger(int loglevel, const char *fmt, ...);

#endif

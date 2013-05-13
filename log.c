#include <printf.h>
#include <stdarg.h>
#include <stdio.h>

int logger(int loglevel, char *fmt, ...) {
	return 0;
	char text[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	fprintf(stderr, "%s\n", text);
}

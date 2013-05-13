#ifndef __INCLUDED_GETSH_HTTP
#define __INCLUDED_GETSH_HTTP

#include <event.h>
#include <event2/http.h>
#include "frontend.h"

extern struct evhttp *httpd;

extern void add_channel(const char *name, int sid, struct tune t);

#endif

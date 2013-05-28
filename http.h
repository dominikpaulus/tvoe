#ifndef __INCLUDED_GETSH_HTTP
#define __INCLUDED_GETSH_HTTP

#include <event.h>
#include <event2/http.h>
#include "frontend.h"

extern struct evhttp *httpd;

/**
 * Adds URL handlers for the specified channel. On client request, the HTTP
 * module will tune to the specified transponder and send the service "sid" to
 * the client
 * @param name Human-readable channel name
 * @param sid Service ID
 * @param t Transponder to tune to
 */
extern void http_add_channel(const char *name, int sid, struct tune t);

#endif

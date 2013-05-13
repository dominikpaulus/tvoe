#include <glib.h>
#include "frontend.h"
#include "log.h"
#include "http.h"

struct evhttp *httpd;

static void http_closecb(struct evhttp_connection *req, void *ptr) {
	struct tune *t = (struct tune *) ptr;
	release_frontend(*t);
	printf("Connection closed\n");
}

static void http_callback(struct evhttp_request *req, void *ptr) {
	struct tune *t = (struct tune *) ptr;
	logger(LOG_INFO, "New request for SID %d", t->sid);
	printf("%d\n", subscribe_to_frontend(*t));
	evhttp_send_reply_start(req, 200, "OK");
	struct evbuffer *foo = evbuffer_new();
	evbuffer_add(foo, "asdf", 4);
	evhttp_send_reply_chunk(req, foo);
	evhttp_connection_set_closecb(evhttp_request_get_connection(req), http_closecb, ptr);
}

void add_channel(const char *name, int sid, struct tune t) {
	char text[128];
	snprintf(text, 128, "/by-sid/%d", sid);
	printf("New channel: %s, SID: %d, URL: %s, tune: ...\n", name, sid, text);
	struct tune *ptr = g_slice_alloc(sizeof(struct tune));
	*ptr = t;
	evhttp_set_cb(httpd, text, http_callback, ptr);
}

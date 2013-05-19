#include <glib.h>
#include "frontend.h"
#include "log.h"
#include "mpeg.h"
#include "http.h"

struct evhttp *httpd;
struct output {
	struct tune t;
	void *handle;
};

static void http_sendcb(struct evbuffer *buf, void *ptr) {
	evhttp_send_reply_chunk(ptr, buf);
}

static void http_closecb(struct evhttp_connection *req, void *ptr) {
	struct output *c = (struct output *) ptr;
	unregister_client(ptr);
	release_frontend(c->t);
	logger(LOG_DEBUG, "Dropping HTTP connection");
}

static void http_callback(struct evhttp_request *req, void *ptr) {
	struct output *c = ptr;
	logger(LOG_INFO, "New request for SID %d", c->t.sid);
	if(subscribe_to_frontend(c->t) < 0) {
		logger(LOG_NOTICE, "Unable to fulfill request: subscribe_to_frontend() failed");
		evhttp_send_reply(req, HTTP_SERVUNAVAIL, "No available tuner", NULL);
		return;
	}
	register_client(c->t.sid, http_sendcb, req); // Never fails
	evhttp_send_reply_start(req, 200, "OK");
	evhttp_connection_set_closecb(evhttp_request_get_connection(req), http_closecb, ptr);
}

void add_channel(const char *name, int sid, struct tune t) {
	char text[128];
	snprintf(text, 128, "/by-sid/%d", sid);
	//printf("New channel: %s, SID: %d, URL: %s, tune: ...\n", name, sid, text);
	struct output *ptr = g_slice_alloc(sizeof(struct output));
	ptr->t = t;
	evhttp_set_cb(httpd, text, http_callback, ptr);
}

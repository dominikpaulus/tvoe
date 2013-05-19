#include <glib.h>
#include <fcntl.h>
#include "frontend.h"
#include "log.h"
#include "mpeg.h"
#include "http.h"

struct evhttp *httpd;
struct output {
	struct tune *t;
	void *handle;
};

//int fd;

static void http_sendcb(struct evbuffer *buf, void *ptr) {
	//evbuffer_write(buf, fd);
	evhttp_send_reply_chunk(ptr, buf);
	// TODO: Drop clients with large output buffers
}

static void http_closecb(struct evhttp_connection *req, void *ptr) {
	struct output *c = (struct output *) ptr;
	unregister_client(c->handle);
	release_frontend(*c->t);
	logger(LOG_DEBUG, "Dropping HTTP connection");
	g_slice_free1(sizeof(struct output), ptr);
}

static void http_callback(struct evhttp_request *req, void *ptr) {
	struct tune *t = ptr;
	logger(LOG_INFO, "New request for SID %d", t->sid);
	if(subscribe_to_frontend(*t) < 0) {
		logger(LOG_NOTICE, "Unable to fulfill request: subscribe_to_frontend() failed");
		evhttp_send_reply(req, HTTP_SERVUNAVAIL, "No available tuner", NULL);
		return;
	}
	struct output *c = g_slice_alloc(sizeof(struct output));
	c->t = t;
	//fd = open("tmpfile", O_CREAT | O_WRONLY);
	c->handle = register_client(t->sid, http_sendcb, req); // Never fails
	evhttp_send_reply_start(req, 200, "OK");
	evhttp_connection_set_closecb(evhttp_request_get_connection(req), http_closecb, c);
}

void add_channel(const char *name, int sid, struct tune t) {
	char text[128];
	snprintf(text, sizeof(text), "/by-sid/%d", sid);
	//printf("New channel: %s, SID: %d, URL: %s, tune: ...\n", name, sid, text);
	struct tune *ptr = g_slice_alloc(sizeof(struct tune));
	*ptr = t;
	evhttp_set_cb(httpd, text, http_callback, ptr);
}

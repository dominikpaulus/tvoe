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
	struct event *timer;
};

// Called by libevent on connection close
static void http_closecb(struct evhttp_connection *req, void *ptr) {
	struct output *c = (struct output *) ptr;
	unregister_client(c->handle);
	event_del(c->timer);
	event_free(c->timer);
	logger(LOG_DEBUG, "Dropping HTTP connection");
	g_slice_free1(sizeof(struct output), ptr);
}

/* 
 * Invoked every second to make sure our buffers don't overflow.
 * We don't do this on every packet sent to save CPU time, thus,
 * we need a timer.
 */
static void http_check_bufsize(evutil_socket_t fd, short what, void *arg) {
	struct evhttp_connection *conn = evhttp_request_get_connection(arg);
	size_t len = evbuffer_get_length(bufferevent_get_output(evhttp_connection_get_bufferevent(conn)));
	size_t len2 = evbuffer_get_length(evhttp_request_get_output_buffer(arg));
	if(len > 1024 * 1024 * 10 || len2 > 1024 * 1024 * 10) {
		logger(LOG_ERR, "HTTP overflow");
		evhttp_send_reply_end(arg);
		evhttp_connection_free(conn);
		return;
	}
}

/*
 * Invoked by libevent when new HTTP request is received
 */
static void http_callback(struct evhttp_request *req, void *ptr) {
	struct tune *t = ptr;
	void *handle;
	logger(LOG_INFO, "New request for SID %d", t->sid);
	if(!(handle = register_client(*t, (void (*) (void *, struct evbuffer *)) evhttp_send_reply_chunk, req))) {
		logger(LOG_NOTICE, "Unable to fulfill request: register_client() failed");
		evhttp_send_reply(req, HTTP_SERVUNAVAIL, "No available tuner", NULL);
		return;
	}
	struct output *c = g_slice_alloc(sizeof(struct output));
	c->handle = handle;
	c->t = t;

	evhttp_send_reply_start(req, 200, "OK");
	evhttp_connection_set_closecb(evhttp_request_get_connection(req), http_closecb, c);

	/* Add bufsize timer */
	c->timer = event_new(NULL, -1, EV_PERSIST, http_check_bufsize, req);
	struct timeval tv = { 1, 0 };
	event_add(c->timer, &tv);
}

void add_channel(const char *name, int sid, struct tune t) {
	char text[128];
	snprintf(text, sizeof(text), "/by-sid/%d", sid);
	struct tune *ptr = g_slice_alloc(sizeof(struct tune));
	*ptr = t;
	evhttp_set_cb(httpd, text, http_callback, ptr);
}

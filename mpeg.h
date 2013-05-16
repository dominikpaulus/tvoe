#ifndef __INCLUDED_GETSH_MPEG
#define __INCLUDED_GETSH_MPEG

#include <stddef.h>
#include <event.h>
#include "frontend.h"

#define MAX_PID 0x1fff

void mpeg_init();
void *register_transponder(struct tune s);
void unregister_transponder(void *handle);
void register_client(unsigned int pid, void (*cb) (struct evbuffer *, void *), void *ptr);
void unregister_client(unsigned int sid, void (*cb) (struct evbuffer *, void *), void *ptr);
void handle_input(void *handle, unsigned char *data, size_t len);

#endif

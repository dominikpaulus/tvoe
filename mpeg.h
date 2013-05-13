#ifndef __INCLUDED_GETSH_MPEG
#define __INCLUDED_GETSH_MPEG

#include <stddef.h>
#include <event.h>
#include "frontend.h"

void *register_transponder(struct tune s);
void unregister_transponder(void *handle);
int register_client(int pid, void (*cb) (struct evbuffer *, void *), void *ptr);
void handle_input(void *handle, unsigned char *data, size_t len);

#endif

#ifndef __INCLUDED_GETSH_MPEG
#define __INCLUDED_GETSH_MPEG

#include <stddef.h>
#include "frontend.h"

void *register_transponder(struct tune s);
void unregister_transponder(void *handle);
void handle_input(unsigned char *data, size_t len);

#endif

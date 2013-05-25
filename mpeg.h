#ifndef __INCLUDED_GETSH_MPEG
#define __INCLUDED_GETSH_MPEG

#include <stddef.h>
#include <event.h>
#include "frontend.h"

#define MAX_PID 0x2000

/**
 * Register new MPEG-TS handler on new transponder.
 * @return Pointer to an opaque struct referencing this handler
 */
void *register_transponder(void);
/**
 * Destroy a MPEG-TS handler.
 * @param handle Handle of the parser to be destroyed
 */
void unregister_transponder(void *handle);
/**
 * Callback for new MPEG-TS input data. Called by the frontend module
 * when reading from frontend succeeded and data is ready for parsing.
 * @param handle Handler for this transport stream
 * @param data Pointer to data to be parsed
 * @param len Length of data at "data"
 */
void handle_input(void *handle, unsigned char *data, size_t len);
/**
 * Register new client requesting program "sid". This module will take care of
 * extracting the requested service from the input data stream and generating a
 * new MPEG-TS stream supplied to the client. The specified callback will be
 * invoked every time new data is ready to be sent to the client, ptr is a
 * pointer to an arbitrary data structure that will be provided unchanged to
 * the callback.
 * @param s Requested program
 * @param cb Callback to invoke when new data is ready
 * @param ptr Pointer to be passed to the callback when invoked
 * @return Pointer to client handle, to be passed to unregister_client()
 */
void *register_client(struct tune s, void (*cb) (void *, struct evbuffer *), void *ptr);
/**
 * Deregister a specific client
 * @param ptr Pointer to handle returned by register_client()
 */
void unregister_client(void *ptr);

#endif

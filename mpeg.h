#ifndef __INCLUDED_TVOE_MPEG
#define __INCLUDED_TVOE_MPEG

#include <stddef.h>
#include <event.h>
#include <stdint.h>
#include "frontend.h"

#define MAX_PID 0x2000

/**
 * Callback for new MPEG-TS input data. Called by the frontend module
 * when reading from frontend succeeded and data is ready for parsing.
 * @param handle Handler for this transport stream
 * @param data Pointer to data to be parsed
 * @param len Length of data at "data"
 */
void mpeg_input(void *handle, unsigned char *data, size_t len);
/**
 * Register new client requesting program "sid". This module will take care of
 * extracting the requested service from the input data stream and generating a
 * new MPEG-TS stream supplied to the client. The specified callback will be
 * invoked every time new data is ready to be sent to the client, ptr is a
 * pointer to an arbitrary data structure that will be provided unchanged to
 * the callback.
 * @param s Requested program
 * @param cb Callback to invoke when new data is ready
 * @param timeout_cb Callback to invoke on frontend tune timeout
 * @param ptr Pointer to be passed to the callback when invoked
 * @return Pointer to client handle, to be passed to mpeg_unregister()
 */
void *mpeg_register(struct tune s, void (*cb) (void *, const uint8_t *buf, uint16_t bufsize),
		void (*timeout_cb) (void *), void *ptr);
/**
 * Deregister a specific client
 * @param ptr Pointer to handle returned by mpeg_register()
 */
void mpeg_unregister(void *ptr);
/**
 * Called by the frontend module when tuning times out.
 */
void mpeg_notify_timeout(void *handle);

#endif

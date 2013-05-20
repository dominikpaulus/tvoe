#ifndef __INCLUDED_GETSH_CHANNELS
#define __INCLUDED_GETSH_CHANNELS

/**
 * Parse the channels.conf in "channelsconf" and add the appropriate
 * HTTP callbacks
 */
extern int parse_channels(const char *channelsconf);

#endif

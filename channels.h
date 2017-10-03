#ifndef __INCLUDED_TVOE_CHANNELS
#define __INCLUDED_TVOE_CHANNELS

/**
 * Parse the channels.conf in "channelsconf" and add the appropriate
 * HTTP callbacks
 */
extern int parse_channels(const char *channelsconf);

#endif

#ifndef __INCLUDED_GETSH_FRONTEND
#define __INCLUDED_GETSH_FRONTEND

#include <stdbool.h>

struct tune {
	/** Delivery system type, reserved for future use */
	unsigned int type;
//	union {
	struct {
		/** Delivery system (SYS_DVBS vs SYS_DVBS2) */
		unsigned int delivery_system;
		unsigned int frequency;
		unsigned int symbol_rate;
		/** Polarization. True: horizontal, false: Vertical */
		bool polarization;
	} dvbs;
	/** Service ID requested */
	unsigned int sid;
//	};
};

struct lnb {
	int lof1, lof2, slof;
};

/**
 * Subscribe to a specific transponder. This function increments the user count
 * if there is already a tuner tuned to this transponder, otherwise it selects
 * an idle tuner and tunes to the specified transponder.
 * @param s Struct describing the transponder to tune to
 * @return 0 if successful, <0 on error. Appropiate error message is sent to
 * the application log.
 */
int subscribe_to_frontend(struct tune s);
/**
 * Unsubscribe from a specific transponder. If the user count for the
 * associated frontend gets 0, releases the frontend and appends it to the list
 * of idle tuners.
 * @param s Struct describing the transonder to release
 */
void release_frontend(struct tune s);
/**
 * Add a new DVB-S frontend on /dev/dvb/adapterX/frontendY, X and Y are
 * specified by the caller, and sets the parameters of the attached LNB.
 * No error handling is performed if the frontend does not exists or is in use,
 * subscribe_to_frontend() will fail at some point then. (This behaviour might
 * change in the future)
 * @param adapter Adapter number
 * @param frontend Frontend number
 */
void add_frontend(int adapter, int frontend, struct lnb l);

#endif

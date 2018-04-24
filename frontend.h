#ifndef __INCLUDED_TVOE_FRONTEND
#define __INCLUDED_TVOE_FRONTEND

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
	size_t dmxbuf;
};

/**
 * Tune to a specific transponder. This function selects a new, current unused
 * frontend and tunes to the specified frequency. dvr_callback will be called
 * with the argument "ptr" passed unmodified to the callback
 * @param s Struct describing the transponder to tune to
 * @param ptr Pointer to be passed to the callback function
 * @return Frontend handle to be passed to frontend_release(), NULL
 * on error.
 */
void *frontend_acquire(struct tune s, void *ptr);
/**
 * Release a specific frontend
 * @param ptr Pointer returned by frontend_acquire()
 */
void frontend_release(void *ptr);
/**
 * Add a new DVB-S frontend on /dev/dvb/adapterX/frontendY, X and Y are
 * specified by the caller, and sets the parameters of the attached LNB.
 * No error handling is performed if the frontend does not exists or is in use,
 * subscribe_to_frontend() will fail at some point then. (This behaviour might
 * change in the future)
 * @param adapter Adapter number
 * @param frontend Frontend number
 */
int frontend_add(int adapter, int frontend, struct lnb l);
/**
 * Initialize the frontend management subsystem
 */
void frontend_init(void);

#endif

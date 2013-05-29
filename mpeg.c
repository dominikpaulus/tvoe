#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <bitstream/mpeg/ts.h>
#include <bitstream/mpeg/psi.h>
#include <bitstream/mpeg/psi/pmt_print.h>
#include <glib.h>
#include <assert.h>
#include "mpeg.h"
#include "log.h"

/* 
 * A lot of code in this file is based on biTstream examples.
 * TODO: Licensing information.
 */

/*
 * Struct describing one specific client and the associated callbacks
 */
struct client {
	/** sid requested by this client */
	int sid;
	/** As we send different PATs to different clients, we have a per-client
	 * PAT continuity counter */
	uint8_t pid0_cc;
	/** Associated transponder */
	struct transponder *t;
	/** Callback for MPEG-TS input */
	void (*cb) (void *, struct evbuffer *);
	/** Callback to call on timeout */
	void (*timeout_cb) (void *);
	/** Argument to supply to the callback functions */
	void *ptr;
};
struct pid_info {
	/** true if we are interested in this PID, i.e., we should parse
	 * tables transmitted over this PID */
	bool parse;
	int8_t last_cc;
	uint8_t *psi_buffer;
	uint16_t psi_buffer_used;
	GSList *callback;
};
struct transponder {
	/** Transport stream ID. Taken over as part of the PAT */
	uint16_t tsid;
	/** User refcount */
	int users;
	/** Handle for the associated frontend */
	void *frontend_handle;
	/** Current frequency */
	struct tune in;
	struct pid_info pids[MAX_PID];
	/** Used as temporary data storage for data sent to the client */
	struct evbuffer *out;
	/** List of clients subscribed to this transponder */
	GSList *clients;
};
static GSList *transponders;

/* Helper function for send_pat() */
static void output_psi_section(struct transponder *a, struct client *c, uint8_t *section, uint16_t pid, uint8_t *cc) {
    uint16_t section_length = psi_get_length(section) + PSI_HEADER_SIZE;
    uint16_t section_offset = 0;
    do {
        uint8_t ts[TS_SIZE];
        uint8_t ts_offset = 0;
        memset(ts, 0xff, TS_SIZE);

        psi_split_section(ts, &ts_offset, section, &section_offset);

        ts_set_pid(ts, pid);
        ts_set_cc(ts, *cc);
        (*cc)++;
        *cc &= 0xf;

        if (section_offset == section_length)
            psi_split_end(ts, &ts_offset);


		evbuffer_add(a->out, ts, TS_SIZE);
		c->cb(c->ptr, a->out);
    } while (section_offset < section_length);
}

static void send_pat(struct transponder *a, struct client *c, uint16_t sid, uint16_t pid) {
	uint8_t *pat = psi_allocate();
	uint8_t *pat_n, j = 0;

    pat_init(pat);
	pat_set_tsid(pat, 0);
    psi_set_section(pat, 0);
    psi_set_lastsection(pat, 0);
    psi_set_version(pat, 0);
    psi_set_current(pat);
    psi_set_length(pat, PSI_MAX_SIZE);

    pat_n = pat_get_program(pat, j++);
    patn_init(pat_n);
    patn_set_program(pat_n, sid);
    patn_set_pid(pat_n, pid);

    // Set correct PAT length
    pat_n = pat_get_program(pat, j); // Get offset of the end of last program
    pat_set_length(pat, pat_n - pat - PAT_HEADER_SIZE);
    psi_set_crc(pat);

	output_psi_section(a, c, pat, PAT_PID, &c->pid0_cc);

    free(pat);
}

/* Helper function to register all clients in it as callbacks for PID pid
 * on transponder a */
static void register_callback(GSList *it, struct transponder *a, uint16_t pid) {
	// Loop over all supplied clients and add them if requested
	for(; it; it = g_slist_next(it)) {
		GSList *it2 = a->pids[pid].callback;
		for(; it2 != NULL; it2 = g_slist_next(it2)) {
			if(it2->data == it->data)
				break;
		}
		if(it2) // Client already registered
			continue;
		a->pids[pid].callback =
			g_slist_prepend(a->pids[pid].callback, it->data);
	}
}

/*
 * Process a new parsed PMT. Map PIDs to corresponding SIDs.
 * @param p Pointer to struct pmt_handle
 */
static void pmt_handler(struct transponder *a, uint16_t pid, uint8_t *section) {
	int j;

	if(!pmt_validate(section)) {
		//logger(LOG_NOTICE, "Invalid PMT received on PID %u", pid);
		free(section);
		return;
	}

	uint8_t *es;
	// Register callback for all elementary streams for this SID
	for(j = 0; (es = pmt_get_es(section, j)); j++)
		register_callback(a->pids[pid].callback, a, pmtn_get_pid(es));
	// ... and for the PCR
	register_callback(a->pids[pid].callback, a, pmt_get_pcrpid(section));

	free(section);
}

/*
 * Process a new parsed PAT on input stream. Add PMT parsers for all referenced
 * channels, if necessary.
 * @param p Pointer to struct mpeg_handle
 */
static void pat_handler(struct transponder *a, uint16_t pid, uint8_t *section) {
	PSI_TABLE_DECLARE(new_pat);
	uint8_t last_section;
	int i;

	if(!pat_validate(section)) {
		free(section);
		return;
	}

	psi_table_init(new_pat);
	if(!psi_table_section(new_pat, section) || !psi_table_validate(new_pat)
			|| !pat_table_validate(new_pat)) {
		psi_table_free(new_pat);
		return;
	}

	last_section = psi_table_get_lastsection(new_pat);
	for(i = 0; i <= last_section; i++) {
		uint8_t *cur = psi_table_get_section(new_pat, i);
		const uint8_t *program;
		int j;

		a->tsid = pat_get_tsid(cur);

		/* 
		 * For every proram in this PAT, check whether we have clients
		 * that request it. Add callbacks for them, if necessary.
		 */
		for(j = 0; (program = pat_get_program(cur, j)); j++) {
			uint16_t cur_sid = patn_get_program(program);

			a->pids[patn_get_pid(program)].parse = true; // We always parse all PMTs

			for(GSList *it = a->clients; it != NULL; it = g_slist_next(it)) {
				struct client *c = it->data;
				if(c->sid != cur_sid)
					continue;

				// Send new PAT to this client
				send_pat(a, c, cur_sid, patn_get_pid(program));

				/* Check whether callback for PMT is already installed */
				GSList *it2;
				for(it2 = a->pids[patn_get_pid(program)].callback;
						it2 != NULL; it2 = g_slist_next(it2)) {
					if(it2->data == c)
						break;
				}
				if(it2) // Callback already registered
					continue;
				a->pids[patn_get_pid(program)].callback =
					g_slist_prepend(a->pids[patn_get_pid(program)].callback, c);
			}
			//logger(LOG_DEBUG, "%d -> %d", patn_get_program(program),
			//		patn_get_pid(program)); 
		}
	}

	/* Additionally to the PIDs defined in the PMT, we also forward the EPG
	 * informations to all clients. They always have PID 18. */
	register_callback(a->clients, a, 18);

	psi_table_free(new_pat);

	return;
}

static void handle_section(struct transponder *a, uint16_t pid, uint8_t *section) {
	uint8_t table_pid = psi_get_tableid(section);
	if(!psi_validate(section)) {
		free(section);
		return;
	}
	switch(table_pid) {
		case PAT_TABLE_ID:
			pat_handler(a, pid, section);
			break;
		case PMT_TABLE_ID:
			pmt_handler(a, pid, section);
			break;
		default:
			free(section);
	}
}

void mpeg_input(void *ptr, unsigned char *data, size_t len) {
	struct transponder *a = ptr;

	if(len % TS_SIZE) {
		logger(LOG_NOTICE, "Unaligned MPEG-TS packets received, dropping.");
		return;
	}

	for(int i=0; i < len; i+=TS_SIZE) {
		uint8_t *cur = data + i;
		uint16_t pid = ts_get_pid(cur);
		GSList *it;

		if(pid >= MAX_PID - 1)
			continue;

		// Mostly c&p from biTstream examples

		// Send packet to clients
		for(it = a->pids[pid].callback; it != NULL; it = g_slist_next(it)) {
			struct client *c = it->data;
			evbuffer_add(a->out, cur, TS_SIZE);
			c->cb(c->ptr, a->out);
		}

		if(!a->pids[pid].parse)
			continue;

		if(ts_check_duplicate(ts_get_cc(cur), a->pids[pid].last_cc) ||
				!ts_has_payload(cur))
			continue;
		if(ts_check_discontinuity(ts_get_cc(cur), a->pids[pid].last_cc))
			psi_assemble_reset(&a->pids[pid].psi_buffer, &a->pids[pid].psi_buffer_used);
	
		a->pids[pid].last_cc = ts_get_cc(cur);

		const uint8_t *payload = ts_section(cur);
		uint8_t length = data + TS_SIZE - payload;

		if(!psi_assemble_empty(&a->pids[pid].psi_buffer, &a->pids[pid].psi_buffer_used)) {
			uint8_t *section = psi_assemble_payload(&a->pids[pid].psi_buffer,
					&a->pids[pid].psi_buffer_used, &payload, &length);
			if(section)
				handle_section(a, pid, section);
		}

		payload = ts_next_section(cur);
		length = cur + TS_SIZE - payload;

		while(length) {
			uint8_t *section = psi_assemble_payload(&a->pids[pid].psi_buffer,
					&a->pids[pid].psi_buffer_used, &payload, &length);
			if(section)
				handle_section(a, pid, section);
		}
	}
}

void mpeg_notify_timeout(void *handle) {
	struct transponder *t = handle;
	frontend_release(t->frontend_handle);
	t->frontend_handle = frontend_acquire(t->in, t);
	if(!t->frontend_handle) {
		logger(LOG_ERR, "Unable to acquire transponder while looking for replacement after timeout");
		GSList *copy = g_slist_copy(t->clients);
		for(GSList *it = copy; it; it = g_slist_next(it)) {
			struct client *scb = it->data;
			scb->timeout_cb(scb->ptr);
		}
		g_slist_free(copy);
	}
	logger(LOG_NOTICE, "Switched frontend after timeout");
}

void *mpeg_register(struct tune s, void (*cb) (void *, struct evbuffer *),
		void (*timeout_cb) (void *), void *ptr) {
	struct client *scb = g_slice_alloc(sizeof(struct client));
	scb->cb = cb;
	scb->timeout_cb = timeout_cb;
	scb->ptr = ptr;
	scb->sid = s.sid;
	scb->pid0_cc = 0;

	/* Check whether we are already receiving a multiplex containing
	 * the requested program */
	GSList *it = transponders;
	for(; it != NULL; it = g_slist_next(it)) {
		struct transponder *t = it->data;
		struct tune in = t->in;
		if(in.dvbs.delivery_system == s.dvbs.delivery_system &&
				in.dvbs.symbol_rate == s.dvbs.symbol_rate &&
				in.dvbs.frequency == s.dvbs.frequency &&
				in.dvbs.polarization == s.dvbs.polarization) {
			t->users++;
			t->clients = g_slist_prepend(t->clients, scb);
			scb->t = t;
			logger(LOG_DEBUG, "New client on known transponder. New client count: %d",
					t->users);
			return scb;
		}
	}

	/* We aren't, acquire new frontend */
	struct transponder *t = g_slice_alloc(sizeof(struct transponder));
	t->frontend_handle = frontend_acquire(s, t);
	if(!t->frontend_handle) { // Unable to acquire frontend
		g_slice_free1(sizeof(struct transponder), t);
		g_slice_free1(sizeof(struct client), scb);
		return NULL;
	}
	t->in = s;
	t->out = evbuffer_new();
	t->users = 1;
	t->clients = NULL;
	t->clients = g_slist_prepend(t->clients, scb);
	scb->t = t;
	for(int i = 0; i < MAX_PID; i++) {
		t->pids[i].last_cc = 0;
		t->pids[i].callback = NULL;
		psi_assemble_init(&t->pids[i].psi_buffer, &t->pids[i].psi_buffer_used);
	}
	t->pids[0].parse = true; // Always parse the PAT
	transponders = g_slist_prepend(transponders, t);
	return scb;
}

void mpeg_unregister(void *ptr) {
	struct client *scb = ptr;
	struct transponder *t = scb->t;
	t->users--;
	if(!t->users) { // Completely remove transponder
		if(t->frontend_handle)
			frontend_release(t->frontend_handle);
		for(int i = 0; i < MAX_PID; i++) {
			psi_assemble_reset(&t->pids[i].psi_buffer, &t->pids[i].psi_buffer_used);
			g_slist_free(t->pids[i].callback);
		}
		evbuffer_free(t->out);
		g_slice_free1(sizeof(struct client), scb);
		transponders = g_slist_remove(transponders, t);
		g_slice_free1(sizeof(struct transponder), t);
	} else { // Only unregister this client
		/* 
		 * Iterate over all callbacks and remove this client from them.
		 * This is extremely expensive, however, disconnects should be
		 * rather rare. This code should be optimized in the future.
		 */
		for(int i=0; i < MAX_PID; i++)
			t->pids[i].callback = g_slist_remove(t->pids[i].callback, scb);
		t->clients = g_slist_remove(t->clients, scb);
		g_slice_free1(sizeof(struct client), scb);
		logger(LOG_INFO, "Client quitted, new transponder user count: %d",
				t->users);
	}
}

#ifndef __INCLUDED_GETSH_FRONTEND
#define __INCLUDED_GETSH_FRONTEND

struct tune {
	unsigned int type;
//	union {
	struct {
		unsigned int delivery_system;
		unsigned int symbol_rate;
		unsigned int frequency;
		bool polarization; // true: horizontal, false: vertical
	} dvbs;
	unsigned int sid;
//	};
};

struct lnb {
	int lof1, lof2, slof;
};

int subscribe_to_frontend(struct tune s);
void release_frontend(struct tune s);
void add_frontend(int adapter, int frontend, struct lnb l);
void remove_frontend(int adapter, int frontend);

#endif

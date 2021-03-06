/*
 * Initng, a next generation sysvinit replacement.
 * Copyright (C) 2006 Jimmy Wennlund <jimmy.wennlund@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef INITNG_ACTIVE_STATE_H
#define INITNG_ACTIVE_STATE_H

typedef struct a_state_t a_state_h;

#include <initng/list.h>
#include <initng/active_db.h>
#include <initng/is_state.h>

struct a_state_t {
	/* the name of the state in a string, will be printed */
	const char *name;

	/* The long description of the state */
	const char *description;

	/* If this state is set for a service, is it roughly: */
	e_is is;

	/*
	 * This function will be run on service with this state set,
	 * if g.interrupt is set.
	 */
	void (*interrupt) (active_db_h *service);

	/*
	 * This will run directly after a service is set this state.
	 */
	void (*init) (active_db_h *service);

	/*
	 * This function will be run when alarm (timeout) is reached
	 */
	void (*alarm) (active_db_h *service);

	/* The list this struct is in */
	list_t list;
};

/* register */
int initng_active_state_register(a_state_h *state);

#define initng_active_state_unregister(st) \
	initng_list_del(&(st)->list)

/* searching */
a_state_h *initng_active_state_find(const char *state_name);

/* walking */
#define while_active_states(current) \
	initng_list_foreach_rev(current, &g.states.list, list)

#define while_active_states_safe(current) \
	initng_list_foreach_rev_safe(current, safe, &g.states.list, list)

#define while_active_state_hooks(current, state) \
	initng_list_foreach_rev(current, &state->test.list, list)

#define while_active_state_hooks_safe(current, safe, state) \
	initng_list_foreach_rev_safe(current, safe, &state->test.list, list)

#endif /* INITNG_ACTIVE_STATE_H */

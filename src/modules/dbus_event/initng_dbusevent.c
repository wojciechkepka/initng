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

#include <initng.h>

#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/socket.h>
#include <string.h>
#include <assert.h>

#include "initng_dbusevent.h"

static int module_init(void);
static void module_unload(void);

const struct initng_module initng_module = {
	.api_version = API_VERSION,
	.deps = { NULL },
	.init = &module_init,
	.unload = &module_unload
};


void send_to_all(char *buf);

static void astatus_change(s_event * event);
static void check_socket(s_event * event);
static int connect_to_dbus(void);
static void system_state_change(s_event * event);
static void system_pipe_watchers(s_event * event);
static void print_error(s_event * event);

static dbus_bool_t add_dbus_watch(DBusWatch * watch, void *data);
static void rem_dbus_watch(DBusWatch * watch, void *data);
static void toggled_dbus_watch(DBusWatch * watch, void *data);
static void iow_callback(f_module_h * from, e_fdw what);

static void free_dbus_watch_data(void *data);

static void w_handler(s_event * event);

DBusConnection *conn;

typedef struct {
	f_module_h fdw;
	DBusWatch *dbus;
	list_t list;
} initng_dbus_watch;

initng_dbus_watch dbus_watches;

static void w_handler(s_event * event)
{
	s_event_io_watcher_data *data;
	initng_dbus_watch *current = NULL;

	assert(event);
	assert(event->data);

	data = event->data;

	initng_list_foreach(current, &dbus_watches.list, list) {
		switch (data->action) {
		case IOW_ACTION_CLOSE:
			if (current->fdw.fds > 0)
				close(current->fdw.fds);
			break;

		case IOW_ACTION_CHECK:
			if (current->fdw.fds <= 2)
				break;

			/* This is a expensive test, but better safe then
			 * sorry */
			if (!STILL_OPEN(current->fdw.fds)) {
				D_("%i is not open anymore.\n",
				   current->fdw.fds);
				current->fdw.fds = -1;
				break;
			}

			FD_SET(current->fdw.fds, data->readset);
			data->added++;
			break;

		case IOW_ACTION_CALL:
			if (!data->added || current->fdw.fds <= 2)
				break;

			if (!FD_ISSET(current->fdw.fds, data->readset))
				break;

			current->fdw.call_module(&current->fdw, IOW_READ);
			data->added--;
			break;

		case IOW_ACTION_DEBUG:
			if (!data->debug_find_what ||
			    strstr(__FILE__, data->debug_find_what)) {
				initng_string_mprintf(data->debug_out,
					" %i: Used by module: %s\n",
					current->fdw.fds, __FILE__);
			}
			break;
		}
	}
}

/* ------  DBus Watch Handling --------
 *
 * NOTE: if any of the F_, D_ etc macros are called during execution of these
 * functions, bad things happen as these call DBus functions and the DBus
 * code is *not* reentrant
 */

static dbus_bool_t add_dbus_watch(DBusWatch * watch, void *data)
{
	initng_dbus_watch *w =
	    initng_toolbox_calloc(1, sizeof(initng_dbus_watch));

	if (!w) {
		printf("Memory allocation failed\n");
		return FALSE;
	}

	w->fdw.fds = dbus_watch_get_unix_fd(watch);
	w->fdw.call_module = iow_callback;
	w->dbus = watch;
	w->fdw.what = 0;

	dbus_watch_set_data(watch, w, free_dbus_watch_data);
	toggled_dbus_watch(watch, data);	/* to set initial state */

	initng_list_add(&w->list, &dbus_watches.list);

	return TRUE;
}

static void rem_dbus_watch(DBusWatch * watch, void *data)
{
	/*   initng_dbus_watch *w = dbus_watch_get_data(watch);

	   if(w) free_dbus_watch_data(w); */
}

static void toggled_dbus_watch(DBusWatch * watch, void *data)
{
	initng_dbus_watch *w = dbus_watch_get_data(watch);

	w->fdw.what = 0;
	if (dbus_watch_get_enabled(watch)) {
		int flags = dbus_watch_get_flags(watch);

		if (flags & DBUS_WATCH_READABLE)
			w->fdw.what |= IOW_READ;

		if (flags & DBUS_WATCH_WRITABLE)
			w->fdw.what |= IOW_WRITE;

		w->fdw.what |= IOW_ERROR;
	}

}

static void free_dbus_watch_data(void *data)
{
	initng_dbus_watch *w = data;

	assert(w);
	free(w);
}

static void iow_callback(f_module_h * from, e_fdw what)
{
	initng_dbus_watch *w = (initng_dbus_watch *) from;
	int flgs = 0;

	/* TODO - handle DBUS_WATCH_HANGUP ? */

	if (what & IOW_READ)
		flgs |= DBUS_WATCH_READABLE;

	if (what & IOW_WRITE)
		flgs |= DBUS_WATCH_WRITABLE;

	if (what & IOW_ERROR)
		flgs |= DBUS_WATCH_ERROR;

	dbus_watch_handle(w->dbus, flgs);
}

/* --- End DBus watch handling ---- */

static void astatus_change(s_event * event)
{
	active_db_h *service;

	DBusMessage *msg;
	dbus_uint32_t serial = 0;

	assert(event->event_type == &EVENT_STATE_CHANGE);
	assert(event->data);

	service = event->data;

	/* these values will be send */
	const char *service_name = service->name;
	int is = service->current_state->is;
	const char *state_name = service->current_state->name;

	if (!conn)
		return;

	D_("Sending signal with value \"%.10s\" %i \"%.10s\"\n", service_name,
	   is, state_name);

	/* create a signal & check for errors */
	msg = dbus_message_new_signal(OBJECT, INTERFACE, "astatus_change");
	if (!msg) {
		F_("Unable to create ne dbus signal\n");
		return;
	}

	/* Append some arguments to the call */
	if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &service_name,
				      DBUS_TYPE_INT32, &is, DBUS_TYPE_STRING,
				      &state_name, DBUS_TYPE_INVALID)) {
		F_("Unable to append args to dbus signal!\n");
		return;
	}

	/* send the message and flush the connection */
	if (!dbus_connection_send(conn, msg, &serial)) {
		F_("Unable to send dbus signal!\n");
		return;
	}
	// dbus_connection_flush(conn);

	D_("Dbus Signal Sent\n");

	/* free the message */
	dbus_message_unref(msg);
}

static void system_state_change(s_event * event)
{
	e_is state;
	DBusMessage *msg;
	dbus_uint32_t serial = 0;

	assert(event->event_type == &EVENT_SYSTEM_CHANGE);
	assert(event->data);

	state = (e_is) event->data;

	if (!conn)
		return;

	/* create a signal & check for errors */
	msg = dbus_message_new_signal(OBJECT, INTERFACE, "system_state_change");
	if (!msg) {
		F_("Unable to create new dbus signal\n");
		return;
	}

	/* Append some arguments to the call */
	if (!dbus_message_append_args(msg, DBUS_TYPE_INT32, &state,
				      DBUS_TYPE_INVALID)) {
		F_("Unable to append args to dbus signal!\n");
		return;
	}

	/* send the message and flush the connection */
	if (!dbus_connection_send(conn, msg, &serial)) {
		F_("Unable to send dbus signal!\n");
		return;
	}
	//dbus_connection_flush(conn);

	/* free the message */
	dbus_message_unref(msg);

	D_("Dbus Signal Sent\n");
	return;
}

static void system_pipe_watchers(s_event * event)
{
	DBusMessage *msg;
	dbus_uint32_t serial = 0;

	assert(event->event_type == &EVENT_PIPE_WATCHER);
	assert(event->data);

	const char *service_name =
	    ((s_event_pipe_watcher_data *) event->data)->service->name;
	const char *process_name =
	    ((s_event_pipe_watcher_data *) event->data)->process->pt->name;

	char output[100];	//used for ? needs fix

	if (!conn) {
		event->status = HANDLED;
		return;
	}

	/* create a signal & check for errors */
	msg = dbus_message_new_signal(OBJECT, INTERFACE, "system_output");
	if (!msg) {
		F_("Unable to create new dbus signal\n");
		event->status = HANDLED;
		return;
	}

	/* Append some arguments to the call */
	if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &service_name,
				      DBUS_TYPE_STRING, &process_name,
				      DBUS_TYPE_STRING, &output,
				      DBUS_TYPE_INVALID)) {
		F_("Unable to append args to dbus signal!\n");
		event->status = HANDLED;
		return;
	}

	/* send the message and flush the connection */
	if (!dbus_connection_send(conn, msg, &serial)) {
		F_("Unable to send dbus signal!\n");
		event->status = HANDLED;
		return;
	}
	//dbus_connection_flush(conn);

	/* free the message */
	dbus_message_unref(msg);

	D_("Dbus Signal Sent\n");
	event->status = HANDLED;
}

static void print_error(s_event * event)
{
	s_event_error_message_data *data;
	DBusMessage *msg;
	dbus_uint32_t serial = 0;
	va_list va;

	assert(event->event_type == &EVENT_ERROR_MESSAGE);
	assert(event->data);

	data = event->data;
	va_copy(va, data->arg);

	if (!conn)
		return;

	/* create a signal & check for errors */
	msg = dbus_message_new_signal(OBJECT, INTERFACE, "print_error");
	if (!msg) {
		F_("Unable to create new dbus signal\n");
		return;
	}

	/* compose the message */
	char *message = initng_toolbox_calloc(1001, 1);

	vsnprintf(message, 1000, data->format, va);
	va_end(va);

	/* Append some arguments to the call */
	if (!dbus_message_append_args(msg, DBUS_TYPE_INT32, &data->mt,
				      DBUS_TYPE_STRING, &data->file,
				      DBUS_TYPE_STRING, &data->func,
				      DBUS_TYPE_INT32, &data->line,
				      DBUS_TYPE_STRING, &message,
				      DBUS_TYPE_INVALID)) {
		F_("Unable to append args to dbus signal!\n");
		return;
	}

	/* send the message and flush the connection */
	if (!dbus_connection_send(conn, msg, &serial)) {
		F_("Unable to send dbus signal!\n");
		return;
	}
	//dbus_connection_flush(conn);

	/* free the message */
	dbus_message_unref(msg);
	free(message);

	D_("Dbus Signal Sent\n");
}

/*
 * On a SIGHUP, close and reopen the socket.
 */
static void check_socket(s_event * event)
{
	int *signal;

	assert(event->event_type == &EVENT_SIGNAL);

	signal = event->data;

	/* only react on a SIGHUP signal */
	if (*signal != SIGHUP)
		return;

	/* close if open */
	if (conn) {
		dbus_connection_close(conn);
		conn = NULL;
	}

	/* and open again */
	connect_to_dbus();
}

static int connect_to_dbus(void)
{
	DBusError err;

	/* initialise the error value */
	dbus_error_init(&err);

	/* connect to the DBUS system bus, and check for errors */
	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (dbus_error_is_set(&err)) {
		F_("Connection Error (%s)\n", err.message);
		dbus_error_free(&err);
	}

	if (!conn)
		return FALSE;

	dbus_connection_set_watch_functions(conn, add_dbus_watch,
					    rem_dbus_watch,
					    toggled_dbus_watch, NULL, NULL);

	/* register our name on the bus, and check for errors */
	/* int ret = */ dbus_bus_request_name(conn, SOURCE_REQUEST,
				    DBUS_NAME_FLAG_REPLACE_EXISTING, &err);

	/* Make sure no error is set */
	if (dbus_error_is_set(&err)) {
		F_("Name Error (%s)\n", err.message);
		dbus_error_free(&err);
	}

	/*  IF this is set, initng is the owner of initng.signal.source */
	/*if ( ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
	   printf("Could not gain PRIMARY_OWNER of "SOURCE_REQUEST"\n");
	   return(FALSE);
	   } */

	return TRUE;
}

int module_init(void)
{
	connect_to_dbus();

	initng_list_init(&dbus_watches.list);

	/* add the hooks we are monitoring */
	initng_event_hook_register(&EVENT_SIGNAL, &check_socket);
	initng_event_hook_register(&EVENT_STATE_CHANGE, &astatus_change);
	initng_event_hook_register(&EVENT_SYSTEM_CHANGE, &system_state_change);
	initng_event_hook_register(&EVENT_PIPE_WATCHER, &system_pipe_watchers);
	initng_event_hook_register(&EVENT_ERROR_MESSAGE, &print_error);
	initng_event_hook_register(&EVENT_IO_WATCHER, &w_handler);

	/* return happily */
	return TRUE;
}

void module_unload(void)
{
	if (conn) {
		dbus_connection_close(conn);
		conn = NULL;
	}

	initng_event_hook_unregister(&EVENT_SIGNAL, &check_socket);
	initng_event_hook_unregister(&EVENT_STATE_CHANGE, &astatus_change);
	initng_event_hook_unregister(&EVENT_SYSTEM_CHANGE,
				     &system_state_change);
	initng_event_hook_unregister(&EVENT_PIPE_WATCHER,
				     &system_pipe_watchers);
	initng_event_hook_unregister(&EVENT_ERROR_MESSAGE, &print_error);
	initng_event_hook_unregister(&EVENT_IO_WATCHER, &w_handler);
}

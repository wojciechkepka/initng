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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <getopt.h>
#include <sys/socket.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <initng.h>
#include <initng-paths.h>

#include "libngcclient.h"

#include "ansi_colors.h"

#ifdef HAVE_NGE
#include "libngeclient.h"
#endif

char *socket_filename = (char *) SOCKET_4_FILENAME_REAL;

int header_printed = FALSE;
int quiet = FALSE;
int ansi = FALSE;

#define print_out(...); { if(quiet == FALSE) printf(__VA_ARGS__); }

#define C_ERROR "\n  " C_FG_RED " [ERROR] -->> " C_OFF

#define TMP_QUIET if(quiet == FALSE) quiet = 3;
#define TMP_UNQUIET if(quiet == 3) quiet = FALSE;

#ifdef HAVE_NGE
active_row *service_starting_stopping = NULL;

static int service_change(char *service, e_is is, char *state)
{
	if (strcmp(service_starting_stopping->name, service) != 0) {
		/*printf("Dont wanna listen on \"%s\"\n", service); */
		return 1;
	}

	switch (is) {
	case IS_UP:
		printf("\nService \"%s\" is started!\n", service);
		/* Close the event socket, and ngclient_exec() should return */
		return 0;

	case IS_DOWN:
		printf("\nService \"%s\" have stopped!\n", service);
		/* Close the event socket, and ngclient_exec() should return */
		return 0;

	case IS_FAILED:
		printf("\nService \"%s\" have failed!\n", service);
		/* Close the event socket, and ngclient_exec() should
		 * return */
		return 0;
	default:
		printf("\nService \"%s\" is now in state: %s\n", service,
		       state);
	}

	return 1;
}

static void service_output(char *service, char *process, char *output)
{
	if (strcmp(service_starting_stopping->name, service) != 0)
		return;

	fprintf(stdout, "%s", output);
}

static int start_or_stop_command(reply * rep)
{
	nge_connection *c = NULL;
	nge_event *e;
	int go = 1;

	service_starting_stopping = rep->payload;

	/* check what state they are in */
	switch (service_starting_stopping->is) {
	case IS_STARTING:
		print_out("Starting service \"%s\", hang on..\n",
			  service_starting_stopping->name);
		break;

	case IS_STOPPING:
		print_out("Stopping service \"%s\", hang on..\n\n\n",
			  service_starting_stopping->name);
		break;

	case IS_DOWN:
		printf("Service %s is down.\n\n\n",
		       service_starting_stopping->name);
		return FALSE;

	case IS_UP:
		printf("Service %s is up.\n", service_starting_stopping->name);
		return FALSE;

	case IS_FAILED:
		printf("Service \"%s\" previously failed (%s),\nit "
		       "needs to be zaped \"ngc -z %s\", so initng "
		       "will forget the failing state before you are "
		       "able to retry start it.\n",
		       service_starting_stopping->name,
		       service_starting_stopping->state,
		       service_starting_stopping->name);
		return FALSE;

	default:
		print_out("Service has state: %s\n",
			  service_starting_stopping->state);
		return FALSE;
	}

	/*
	 * if the state was starting or stopping, start follow
	 * them by nge.
	 */

	/* open correct socket */
	/* FIXME: this should work with custom sockets path (for debugging) */
	c = ngeclient_connect(NGE_REAL);

	/* if open_socket fails, ngeclient_error is set */
	if (ngeclient_error) {
		fprintf(stderr, "%s\n", ngeclient_error);
		return FALSE;
	}

	if (!c) {
		fprintf(stderr, "NGE connect error\n");
		return FALSE;
	}

	/* do for every event that comes in */
	while (go == 1 && (e = get_next_event(c, 20000))) {
		switch (e->state_type) {
		case SERVICE_STATE_CHANGE:
		case INITIAL_SERVICE_STATE_CHANGE:
			go = service_change(e->payload.service_state_change.
					    service,
					    e->payload.service_state_change.is,
					    e->payload.service_state_change.
					    state_name);
			break;

		case SERVICE_OUTPUT:
			service_output(e->payload.service_output.service,
				       e->payload.service_output.process,
				       e->payload.service_output.output);
			break;

		default:
			break;
		}

		/* This will free all strings got */
		ngeclient_event_free(e);
	}

	/* check for failures */
	if (ngeclient_error) {
		fprintf(stderr, "%s\n", ngeclient_error);
	}

	ngeclient_close(c);
	return TRUE;
}
#endif

static int send_and_handle(const char c, const char *l, const char *opt,
			   int instant)
{
	char *string = NULL;
	reply *rep = NULL;

	/*printf("send_and_handle(%c, %s, %s);\n", c, l, opt); */

	rep = ngcclient_send_command(socket_filename, c, l, opt);

	if (ngcclient_error) {
		print_out("%s\n", ngcclient_error);
		return FALSE;
	}

	if (!rep) {
		print_out("Command failed.\n");
		return FALSE;
	}

	/* print header if not printed before. */
	/* TODO, put initng version from rep here */
	if (header_printed == FALSE && quiet == FALSE) {
		/* print banner - only on terminal */
		if (ansi) {
			print_out(C_FG_LIGHT_BLUE " init" C_FG_LIGHT_RED "NGC"
				  C_FG_LIGHT_BLUE "ontrol (" C_FG_MAGENTA "%s"
				  C_FG_LIGHT_BLUE " )" C_OFF C_FG_LIGHT_RED
				  " by Jimmy Wennlund " C_OFF C_FG_NEON_GREEN
				  "http://www.initng.org/" C_OFF "\n", VERSION);
		}
		header_printed = TRUE;
	}
#ifdef HAVE_NGE
	if (instant == FALSE && quiet == FALSE) {
		/*
		 * there are special commands, where we wanna
		 * initziate nge, and follow the service.
		 */
		if (rep->result.c == 'u' || rep->result.c == 'd') {
			return (start_or_stop_command(rep));
		}
	}
#endif

	/* only print when not quiet */
	if (quiet == FALSE) {
		string = ngcclient_reply_to_string(rep, ansi);
		print_out("\n\n%s\n", string);
		free(string);
	}

	free(rep);
	return TRUE;
}

/* THIS IS MAIN */
int main(int argc, char *argv[])
{
	int instant = FALSE;
	int cc = 1;

	/*
	 * Only on first input from initng, we will print a
	 * initng header with version info, after then
	 * header_printed is true, and probits this.
	 */
	header_printed = FALSE;

	/*
	 * If output to a terminal, turn on ansi colors.
	 */
	if (isatty(1))
		ansi = TRUE;


	/* -f specifies a custom socket path */
	if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'f') {
		if (argc < 3) {
			print_out("ERROR: -f needs an argument.\n");
			return 1;
		}
		socket_filename = argv[2];
		cc += 2;
	} else if (getuid() != 0 && ansi) {
		print_out(C_FG_YELLOW "Warning: You need root access to "
			  "communicate with initng.\n" C_OFF);
	}

	/* make sure there are any arguments at all */
	if (argc <= cc) {
		send_and_handle('h', NULL, NULL, instant);
		exit(0);
	}

	/* walk thru all arguments */
	while (argv[cc]) {
		char *opt = NULL;

		/* every fresh start needs a '-' char */
		if (argv[cc][0] != '-') {
			send_and_handle('h', NULL, NULL, instant);
			exit(1);
		}

		/* check that there is a char after the '-' */
		if (!argv[cc][1]) {
			send_and_handle('h', NULL, NULL, instant);
			exit(1);
		}

		/* if next option contains data, but not starting with an '-',
		 * its considered an option */
		if (argv[cc + 1] && argv[cc + 1][0] != '-')
			opt = argv[cc + 1];

		/* if it is an --option */
		if (argv[cc][1] == '-') {
			/* handle local --instant */
			if (strcmp(&argv[cc][2], "instant") == 0) {
				instant = TRUE;
				cc++;
				continue;
			}

			/* handle local --quiet */
			if (strcmp(&argv[cc][2], "quiet") == 0) {
				quiet = TRUE;
				cc++;
				continue;
			}

			if (!send_and_handle('\0', &argv[cc][2], opt, instant))
				exit(1);
		} else {
			if (!send_and_handle(argv[cc][1], NULL, opt, instant))
				exit(1);
		}

		cc++;
		if (opt)
			cc++;
	}

	print_out("\n\n");
	exit(0);
}

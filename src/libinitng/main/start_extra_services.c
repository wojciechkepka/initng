/*
 * Initng, a next generation sysvinit replacement.
 * Copyright (C) 2006 Jimmy Wennlund <jimmy.wennlund@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
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

#include "initng.h"

#include <time.h>							/* time() */
#include <fcntl.h>							/* fcntl() */
#include <sys/un.h>							/* memmove() strcmp() */
#include <sys/wait.h>						/* waitpid() sa */
#include <linux/kd.h>						/* KDSIGACCEPT */
#include <sys/ioctl.h>						/* ioctl() */
#include <stdlib.h>							/* free() exit() */
#include <sys/reboot.h>						/* reboot() RB_DISABLE_CAD */
#include <sys/mount.h>
#include <termios.h>
#include <stdio.h>
#include <sys/klog.h>
#include <errno.h>
#ifdef SELINUX
#include <selinux/selinux.h>
#include <selinux/get_context_list.h>
#endif
#include "initng_global.h"
#include "initng_signal.h"
#include "initng_handler.h"
#include "initng_execute.h"
#include "initng_active_db.h"
#include "initng_load_module.h"
#include "initng_plugin_callers.h"
#include "initng_toolbox.h"
#ifdef HAVE_COREDUMPER
#include <google/coredumper.h>
#endif

#include "initng_main.h"


/*
 * Start initial services
 */
void initng_main_start_extra_services(void)
{
	int i;
	int a_count = 0;			/* counts orders from argv to start */

	initng_main_set_sys_state(STATE_STARTING);
	/* check with argv which service to start initiating */
	for (i = 1; i < g.Argc; i++)
	{
		/* start all services that is +service */
		if (g.Argv[i][0] != '+')
			continue;

		/* if succeed to load this service */
		if (initng_handler_start_new_service_named(g.Argv[i] + 1))
			a_count++;
		else
			F_(" Requested service \"%s\", could not be executed!\n",
			   g.Argv[i]);
	}
}

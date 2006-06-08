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

#include <initng.h>

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
#include <sys/un.h>
#include <assert.h>

#include <initng_global.h>
#include <initng_active_state.h>
#include <initng_active_db.h>
#include <initng_process_db.h>
#include <initng_handler.h>
#include <initng_active_db.h>
#include <initng_toolbox.h>
#include <initng_plugin_hook.h>
#include <initng_load_module.h>
#include <initng_plugin_callers.h>
#include <initng_error.h>
#include <initng_plugin.h>
#include <initng_static_states.h>
#include <initng_control_command.h>
#include <initng_fork.h>
#include <initng_common.h>
#include <initng_string_tools.h>

#ifdef SERVICE_CACHE
#include <initng_service_cache.h>
#endif

#include <initng-paths.h>

#include "initng_service_file.h"

INITNG_PLUGIN_MACRO;

#ifdef GLOBAL_SOCKET
static void bp_incomming(f_module_h * from, e_fdw what);
static int bp_open_socket(void);
static void bp_check_socket(int signal);
static void bp_closesock(void);
#endif

static void bp_handle_client(int fd);
static void bp_new_active(bp_rep * rep, const char *type,
						  const char *service, const char *from_file);
static void bp_set_variable(bp_rep * rep, const char *service,
							const char *vartype, const char *varname,
							const char *value);
static void bp_get_variable(bp_rep * rep, const char *service,
							const char *vartype, const char *varname);
static void bp_done(bp_rep * rep, const char *service);
static void bp_abort(bp_rep * rep, const char *service);

static void handle_killed(active_db_h * service, process_h * process);

#define SOCKET_4_ROOTPATH "/dev/initng"

a_state_h PARSING = { "PARSING", "This is service is parsing by service_file.", IS_STARTING, NULL, NULL, NULL };
a_state_h REDY_TO_START = { "REDY_TO_START", "This service is finished loading.", IS_DOWN, NULL, NULL, NULL };
a_state_h PARSE_FAIL = { "PARSE_FAIL", "This parse process failed.", IS_FAILED, NULL, NULL, NULL };

/* put null to the kill_handler here */
ptype_h parse = { "parse-process", &handle_killed };
stype_h unset = { "unset", "Service type is not set yet, are still parsing.", FALSE, NULL, NULL, NULL };

/*
   In the Linux implementation, sockets which are visible in the file system
   honour the permissions of the directory they are in.  Their owner, group
   and their permissions can be changed.  Creation of a new socket will
   fail if the process does not have write and search (execute) permission
   on the directory the socket is created in.  Connecting to the socket
   object requires read/write permission.  This behavior differs from  many
   BSD derived systems which ignore permissions for Unix sockets.  Portable
   programs should not rely on this feature for security.
 */


/* globals */
struct stat sock_stat;
#ifdef GLOBAL_SOCKET
f_module_h bpf = { &bp_incomming, FDW_READ, -1 };
#endif

#define RSCV() (TEMP_FAILURE_RETRY(recv(fd, &req, sizeof(bp_req), 0)))
#define SEND() send(fd, &rep, sizeof(bp_rep), 0)


static void bp_handle_client(int fd)
{
	bp_req req;
	bp_rep rep;
	int r;

	D_("Got connection\n");
	memset(&req, 0, sizeof(bp_req));
	memset(&rep, 0, sizeof(bp_rep));

	/* use file descriptor, because fread hangs here? */
	r = RSCV();

	/* make sure it has not closed */
	if (r == 0)
	{
		/*printf("Closing %i.\n", fd); */
		close(fd);
		return;
	}

	if (r != (signed) sizeof(bp_req))
	{
		F_("Could not read incomming service_file req.\n");
		strcpy(rep.message, "Unable to read request");
		rep.success = FALSE;
		SEND();
		return;
	}
	D_("Got a request: ver: %i, type: %i\n", req.version, req.request);

	/* check protocol version match */
	if (req.version != SERVICE_FILE_VERSION)
	{
		strcpy(rep.message, "Bad protocol version");
		rep.success = FALSE;
		SEND();
		return;
	}

	/* handle by request type */
	switch (req.request)
	{
		case NEW_ACTIVE:
			bp_new_active(&rep, req.u.new_active.type,
						  req.u.new_active.service,
						  req.u.new_active.from_file);
			break;
		case SET_VARIABLE:
			bp_set_variable(&rep, req.u.set_variable.service,
							req.u.set_variable.vartype,
							req.u.set_variable.varname,
							req.u.set_variable.value);
			break;
		case GET_VARIABLE:
			bp_get_variable(&rep, req.u.get_variable.service,
							req.u.get_variable.vartype,
							req.u.get_variable.varname);
			break;
		case DONE:
			bp_done(&rep, req.u.done.service);
			break;
		case ABORT:
			bp_abort(&rep, req.u.abort.service);
			break;
		default:
			break;
	}

	/* send the reply */
	SEND();
}

static void bp_new_active(bp_rep * rep, const char *type, const char *service,
						  const char *from_file)
{
	active_db_h *new_active;
	stype_h *stype;

	D_("bp_new_active(%s, %s)\n", type, service);


	/* find service type */
	stype = initng_service_type_get_by_name(type);
	if (!stype)
	{
		strcpy(rep->message, "Unable to find servicetype \"");
		strncat(rep->message, type, 500);
		strcat(rep->message, "\" .");
		rep->success = FALSE;
		return;
	}

	/* look for existing */
	new_active = initng_active_db_find_by_exact_name(service);

	/* check for duplet, not parsing */
	if (new_active && new_active->current_state != &PARSING)
	{
		strcpy(rep->message, "Duplet found.");
		rep->success = FALSE;
		return;
	}

	/* if not found, create new service */
	if (!new_active)
	{
		/* create new service */
		new_active = initng_active_db_new(service);
		if (!new_active)
			return;

		/* set type */
		new_active->current_state = &PARSING;
#ifdef SERVICE_CACHE
		new_active->from_service = &NO_CACHE;
#endif

		/* register it */
		if (!initng_active_db_register(new_active))
		{
			initng_active_db_free(new_active);
			return;
		}
	}

	/* save orgin */
	set_string(&FROM_FILE, new_active, i_strdup(from_file));

	/* set service type */
	new_active->type = stype;

	/* exit with success */
	rep->success = TRUE;
	return;
}
static void bp_set_variable(bp_rep * rep, const char *service,
							const char *vartype, const char *varname,
							const char *value)
{
	s_entry *type;
	active_db_h *active = initng_active_db_find_by_exact_name(service);

	/* make sure there are filled, or NULL, these variables are not strictly requierd */
	if (!varname || strlen(varname) < 1)
		varname = NULL;
	if (!value || strlen(value) < 1)
		value = NULL;

	/* but these are */
	if (strlen(service) < 1)
	{
		strcpy(rep->message, "Service missing.");
		rep->success = FALSE;
		return;
	}
	if (strlen(vartype) < 1)
	{
		strcpy(rep->message, "Vartype missing.");
		rep->success = FALSE;
		return;
	}

	D_("bp_set_variable(%s, %s, %s, %s)\n", service, vartype, varname, value);
	if (!active)
	{
		strcpy(rep->message, "Service \"");
		strcat(rep->message, service);
		strcat(rep->message, "\" not found.");
		rep->success = FALSE;
		return;
	}

	if (active->current_state != &PARSING)
	{
		strcpy(rep->message, "Please dont edit finished services.");
		rep->success = FALSE;
		return;
	}

	type = initng_service_data_type_find(vartype);
	if (!type)
	{
		strcpy(rep->message, "Variable entry \"");
		strcat(rep->message, vartype);
		strcat(rep->message, "\" not found.");
		rep->success = FALSE;
		return;
	}

	/* now we now if variable is required or not */
	if (!value)
	{
		if ((type->opt_type != SET && type->opt_type != VARIABLE_SET))
		{
			strcpy(rep->message, "Value missing.");
			rep->success = FALSE;
			return;
		}
	}

	switch (type->opt_type)
	{
		case STRING:
		case VARIABLE_STRING:
			set_string_var(type, varname ? i_strdup(varname) : NULL, active,
						   i_strdup(value));
			D_("string type - %s %s\n", type->opt_name, value);
			break;
		case STRINGS:
		case VARIABLE_STRINGS:
			{
				char *new_st = NULL;

				while ((new_st = st_dup_next_word(&value)))
				{
					set_another_string_var(type,
										   varname ? i_strdup(varname) : NULL,
										   active, new_st);
				}
			}
			D_("strings type\n");
			break;
		case SET:
		case VARIABLE_SET:
			D_("set type\n");
			set_var(type, varname ? i_strdup(varname) : NULL, active);
			break;
		case INT:
		case VARIABLE_INT:
			set_int_var(type, varname ? i_strdup(varname) : NULL, active,
						atoi(value));
			D_("int type\n");
			break;
		default:
			strcpy(rep->message, "Unknown data type.");
			rep->success = FALSE;
			return;
	}


	rep->success = TRUE;
	return;
}
static void bp_get_variable(bp_rep * rep, const char *service,
							const char *vartype, const char *varname)
{
	s_entry *type;
	active_db_h *active = initng_active_db_find_by_exact_name(service);

	/* This one is not required */
	if (strlen(varname) < 1)
		varname = NULL;

	/* but these are */
	if (strlen(service) < 1 || strlen(vartype) < 1)
	{
		strcpy(rep->message, "Variables missing.");
		rep->success = FALSE;
		return;
	}

	D_("bp_get_variable(%s, %s, %s)\n", service, vartype, varname);
	if (!active)
	{
		strcpy(rep->message, "Service \"");

		strncat(rep->message, service, 500);
		strcat(rep->message, "\" not found.");
		rep->success = FALSE;
		return;
	}

	type = initng_service_data_type_find(vartype);
	if (!type)
	{
		strcpy(rep->message, "Variable entry not found.");
		rep->success = FALSE;
		return;
	}

	/* depening on data type */
	switch (type->opt_type)
	{
		case STRING:
		case VARIABLE_STRING:
			strncpy(rep->message, get_string_var(type, varname, active),
					1024);
			break;
		case STRINGS:
		case VARIABLE_STRINGS:
			/* todo, will only return one */
			{
				s_data *itt = NULL;
				const char *tmp = NULL;

				while ((tmp =
						get_next_string_var(type, varname, active, &itt)))
				{
					if (!rep->message[0])
					{
						strcpy(rep->message, tmp);
						continue;
					}
					strcat(rep->message, " ");
					strncat(rep->message, tmp, 1024);
				}
			}
			break;
		case SET:
		case VARIABLE_SET:
			if (is_var(type, varname, active))
			{
				rep->success = TRUE;
			}
			else
			{
				rep->success = FALSE;
			}
			return;
		case INT:
		case VARIABLE_INT:
			{
				int var = get_int_var(type, varname, active);

				sprintf(rep->message, "%i", var);
			}
			break;
		default:
			strcpy(rep->message, "Unknown data type.");
			rep->success = FALSE;
			return;
	}

	rep->success = TRUE;
	return;
}
static void bp_done(bp_rep * rep, const char *service)
{
	active_db_h *active = initng_active_db_find_by_exact_name(service);

	if (!active)
	{
		strcpy(rep->message, "Service not found.");
		rep->success = FALSE;
		return;
	}

	/* check that type is set */
	if (!active->type || active->type == &unset)
	{
		strcpy(rep->message,
			   "Type not set, please run iregister type service\n");
		rep->success = FALSE;
		return;
	}

	if (active->current_state != &PARSING)
	{
		strcpy(rep->message, "Service is not in PARSING state, cant start.");
		rep->success = FALSE;
		return;
	}

	/* must set to a DOWN state, to be able to start */
	active->current_state = &REDY_TO_START;

	/* start this service */
	rep->success = initng_handler_start_service(active);
	return;
}
static void bp_abort(bp_rep * rep, const char *service)
{
	active_db_h *active = initng_active_db_find_by_exact_name(service);

	if (!active)
	{
		strcpy(rep->message, "Service not found.");
		rep->success = FALSE;
		return;
	}

	if (active->current_state != &PARSING)
	{
		strcpy(rep->message, "Service is not in PARSING state, cant start.");
		rep->success = FALSE;
		return;
	}


	D_("bp_abort(%s)\n", service);

	rep->success = TRUE;
	return;
}

#ifdef GLObAL_SOCKET
/* called by fd hook, when data is no socket */
void bp_incomming(f_module_h * from, e_fdw what)
{
	int newsock;

	/* make a dumb check */
	if (from != &bpf)
		return;

	D_("bp_incomming();\n");
	/* we try to fix socket after every service start
	   if it fails here chances are a user screwed it
	   up, and we shouldn't manually try to fix anything. */
	if (bpf.fds <= 0)
	{
		/* socket is invalid but we were called, call closesocket to make sure it wont happen again */
		F_("accepted client called with bpf.fds %d, report bug\nWill repopen socket.", bpf.fds);
		bp_open_socket();
		return;
	}

	/* create a new socket, for reading */
	if ((newsock = accept(bpf.fds, NULL, NULL)) > 0)
	{
		/* read the data, by the handle_client function */
		bp_handle_client(newsock);

		/* close the socket when finished */
		close(newsock);
		return;
	}

	/* temporary unavailable */
	if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
	{
		W_("errno = EAGAIN!\n");
		return;
	}

	/* This'll generally happen on shutdown, don't cry about it. */
	D_("Error accepting socket %d, %s\n", bpf.fds, strerror(errno));
	bp_closesock();
	return;
}
#endif

#ifdef GLOBAL_SOCKET
/* this will try to open a new socket */
static int bp_open_socket()
{
	/*    int flags; */
	struct sockaddr_un serv_sockname;

	D_("Creating " SOCKET_4_ROOTPATH " dir\n");

	bp_closesock();

	/* Make /dev/initng if it doesn't exist (try either way) */
	if (mkdir(SOCKET_4_ROOTPATH, S_IRUSR | S_IWUSR | S_IXUSR) == -1
		&& errno != EEXIST)
	{
		if (errno != EROFS)
			F_("Could not create " SOCKET_4_ROOTPATH
			   " : %s, may be / fs not mounted read-write yet?, will retry until I succeed.\n",
			   strerror(errno));
		return (FALSE);
	}

	/* chmod root path for root use only */
	if (chmod(SOCKET_4_ROOTPATH, S_IRUSR | S_IWUSR | S_IXUSR) == -1)
	{
		/* path doesn't exist, we don't have /dev yet. */
		if (errno == ENOENT || errno == EROFS)
			return (FALSE);

		F_("CRITICAL, failed to chmod %s, THIS IS A SECURITY PROBLEM.\n",
		   SOCKET_4_ROOTPATH);
	}

	/* Create the socket. */
	bpf.fds = socket(PF_UNIX, SOCK_STREAM, 0);
	if (bpf.fds < 1)
	{
		F_("Failed to init socket (%s)\n", strerror(errno));
		bpf.fds = -1;
		return (FALSE);
	}

	/* Bind a name to the socket. */
	serv_sockname.sun_family = AF_UNIX;

	strcpy(serv_sockname.sun_path, SOCKET_PATH);

	/* Remove old socket file if any */
	unlink(serv_sockname.sun_path);

	/* Try to bind */
	if (bind
		(bpf.fds, (struct sockaddr *) &serv_sockname,
		 (strlen(serv_sockname.sun_path) +
		  sizeof(serv_sockname.sun_family))) < 0)
	{
		F_("Error binding to socket (errno: %d str: '%s')\n", errno,
		   strerror(errno));
		bp_closesock();
		unlink(serv_sockname.sun_path);
		return (FALSE);
	}

	/* chmod socket for root only use */
	if (chmod(serv_sockname.sun_path, S_IRUSR | S_IWUSR) == -1)
	{
		F_("CRITICAL, failed to chmod %s, THIS IS A SECURITY PROBLEM.\n",
		   serv_sockname.sun_path);
		bp_closesock();
		return (FALSE);
	}

	/* store sock_stat for checking if we need to recreate socket later */
	stat(serv_sockname.sun_path, &sock_stat);

	/* Listen to socket */
	if (listen(bpf.fds, 5))
	{
		F_("Error on listen (errno: %d str: '%s')\n", errno, strerror(errno));
		bp_closesock();
		unlink(serv_sockname.sun_path);
		return (FALSE);
	}

	return (TRUE);
}
#endif

#ifdef GLOBAL_SOCKET
/* this will check socket, and reopen on failure */
static void bp_check_socket(int signal)
{
	struct stat st;

	if (signal != SIGHUP)
		return;

	D_("Checking socket\n");

	/* Check if socket needs reopening */
	if (bpf.fds <= 0)
	{
		D_("bpf.fds not set, opening new socket.\n");
		bp_open_socket();
		return;
	}

	/* stat the socket, reopen on failure */
	memset(&st, 0, sizeof(st));
	if (stat(SOCKET_PATH, &st) < 0)
	{
		W_("Stat failed! Opening new socket.\n");
		bp_open_socket();
		return;
	}

	/* compare socket file, with the one that we know, reopen on failure */
	if (st.st_dev != sock_stat.st_dev || st.st_ino != sock_stat.st_ino
		|| st.st_mtime != sock_stat.st_mtime)
	{
		F_("Invalid socket found, reopening\n");
		bp_open_socket();
		return;
	}

	D_("Socket ok.\n");
	return;
}
#endif

#ifdef GLOBAL_SOCKET
static void bp_closesock(void)
{
	/* Check if we need to remove hooks */
	if (bpf.fds < 0)
		return;
	D_("bp_closesock %d\n", bpf.fds);

	/* close socket and set to 0 */
	close(bpf.fds);
	bpf.fds = -1;
}
#endif

static void handle_killed(active_db_h * service, process_h * process)
{
	/* if process returned exit 1 or abow, set fail */
	if (WEXITSTATUS(process->r_code) > 0)
	{
		initng_common_mark_service(service, &PARSE_FAIL);
		initng_process_db_free(process);
		return;
	}

	initng_process_db_free(process);
	initng_handler_start_service(service);
}

static active_db_h *create_new_active(const char *name)
{
	char file[1024] = SCRIPT_PATH "/";
	struct stat fstat;
	active_db_h *new_active;
	process_h *process;
	pipe_h *current_pipe;

	/*printf("create_new_active(%s);\n", name); */
	/*printf("service \"%s\" ", name); */

	/* Make the filename, cutting on first '/' in name */
	{
		int i = 0;

		while (name[i] && i < 500 && name[i] != '/')
			i++;
		strncat(file, name, i);
	}

	/* printf(" parsing file \"%s\"\n", file); */

	/* check so that file exists */
	if (stat(file, &fstat) != 0)
	{
#if 0
		/* Gentoo support disabled for now - doesn't work properly yet */
		strcpy(file, "/etc/init.d/");
		strncat(file, name, 1020 - strlen("/etc/init.d/"));

		if (stat(file, &fstat) != 0)
		{
			/* file not found */
			return (NULL);
		}
#else
		W_("File \"%s\" not found.\n", file);
		return (NULL);
#endif
	}
	
	/* check that it is a file */
	if (!S_ISREG(fstat.st_mode))
	{
		F_("File \"%s\" is not an regular file.\n", file);
		return(NULL);
	}
	
	if (!( fstat.st_mode & S_IXUSR))
	{
		F_("File \"%s\" cant be executed!\n", file);
		return(NULL);
	}

	/* create new service */
	new_active = initng_active_db_new(name);
	if (!new_active)
		return (NULL);

	/* set type */
	new_active->current_state = &PARSING;
#ifdef SERVICE_CACHE
	new_active->from_service = &NO_CACHE;
#endif
	new_active->type = &unset;

	/* register it */
	if (!initng_active_db_register(new_active))
	{
		initng_active_db_free(new_active);
		return (NULL);
	}

	/* create the process */
	process = initng_process_db_new(&parse);

	/* bound to service */
	add_process(process, new_active);

	/* create and unidirectional pipe */
	current_pipe = pipe_new(IN_AND_OUT_PIPE);
	if (current_pipe)
	{
		/* we want this pipe to get fd 3, in the fork */
		current_pipe->targets[0] = 3;
		add_pipe(current_pipe, process);
	}

	/* start parse process */
	if (initng_fork(new_active, process) == 0)
	{
		char *new_argv[3];
		char *new_env[4];

		new_argv[0] = file;
		new_argv[1] = i_strdup("internal_setup");
		new_argv[2] = NULL;

		/* SERVICE=getty/tty1 */
		new_env[0] = i_calloc(strlen(name) + 20, sizeof(char));
		strcpy(new_env[0], "SERVICE=");
		strcat(new_env[0], name);

		/* SERVICE_FILE=/etc/init/getty */
		new_env[1] = i_calloc(strlen(file) + 20, sizeof(char));
		strcpy(new_env[1], "SERVICE_FILE=");
		strcat(new_env[1], file);

		/* NAME=tty1 */
		{
			char * tmp = strrchr(name, '/');
			if(tmp && tmp[0] == '/') tmp++;

			if(tmp && tmp[0])
			{
				new_env[2] = i_calloc(strlen(tmp) + 20, sizeof(char));
				strcpy(new_env[2], "NAME=");
				strcat(new_env[2], tmp);
			} else {
				new_env[2] = i_calloc(strlen(name) + 20, sizeof(char));
				strcpy(new_env[2], "NAME=");
				strcat(new_env[2], name);
			}
		}
	
		new_env[3]=NULL;
		
		execve(new_argv[0], new_argv, new_env);
		_exit(10);
	}

	/* return the newly created */
	return (new_active);
}

static int get_pipe(active_db_h * service, process_h * process, pipe_h * pi)
{
	/*printf("get_pipe(%s, %i, %i);\n", service->name, pi->dir, pi->targets[0]); */

	/* extra check */
	if (pi->dir != IN_AND_OUT_PIPE)
		return (FALSE);

	/* the pipe we opened was on fd 3 */
	if (pi->targets[0] != 3)
		return (FALSE);

	/* handle the client in the same way, as a fifo connected one */
	bp_handle_client(pi->pipe[1]);

	/* return happy */
	return (TRUE);
}

#ifdef USE_LOCALEXEC
static int initng_bash_run(active_db_h * service, process_h * process,
						   const char *exec_name)
{
	pid_t pid_fork;				/* pid got from fork() */

	assert(service);
	assert(service->name);
	assert(process);
	assert(exec_name);

	if ((pid_fork = initng_fork(service, process)) == 0)
	{
		struct stat fstat;		/* file stat storage */
		char *new_argv[3];			/* use only 3 args */
		char *new_env[] = { NULL };				/* use an empty environment */
		char *file;				/* the file to execute from */

		/* get the file path */
		file = get_string(&FROM_FILE, service);

		/* check that it exists */
		if (!file || stat(file, &fstat) != 0)
		{
			printf("Service file not found.\n");
			_exit(1);
		}

		/* execute this */
		new_argv[0] = file;
		new_argv[1] = i_calloc(10 + strlen(exec_name), sizeof(char));
		strcpy(new_argv[1], "internal_");
		strcat(new_argv[1], exec_name);
		new_argv[2] = NULL;

		execve(new_argv[0], new_argv, new_env);

		printf("Error executing!\n");
		_exit(2);
	}

	if (pid_fork > 0)
	{
		process->pid = pid_fork;
		return (TRUE);
	}
	process->pid = 0;
	return (FALSE);
}
#endif


int module_init(int api_version)
{
	D_("module_init(ngc2);\n");
	if (api_version != API_VERSION)
	{
		F_("This module is compiled for api_version %i version and initng is compiled on %i version, won't load this module!\n", API_VERSION, api_version);
		return (FALSE);
	}

#ifdef GLOBAL_SOCKET
	/* zero globals */
	bpf.fds = -1;
	memset(&sock_stat, 0, sizeof(sock_stat));
#endif

	D_("adding hook, that will reopen socket, for every started service.\n");
	initng_process_db_ptype_register(&parse);
#ifdef GLOBAL_SOCKET
	initng_plugin_hook_register(&g.FDWATCHERS, 30, &bpf);
	initng_plugin_hook_register(&g.SIGNAL, 50, &bp_check_socket);
#endif
	initng_plugin_hook_register(&g.NEW_ACTIVE, 50, &create_new_active);
	initng_plugin_hook_register(&g.PIPE_WATCHER, 30, &get_pipe);
	initng_active_state_register(&PARSING);
	initng_active_state_register(&PARSE_FAIL);
#ifdef USE_LOCALEXEC
	initng_plugin_hook_register(&g.LAUNCH, 10, &initng_bash_run);
#endif

#ifdef GLOBAL_SOCKET
	/* do the first socket directly */
	bp_open_socket();
#endif

	return (TRUE);
}

void module_unload(void)
{
	D_("module_unload(ngc2);\n");

#ifdef GLOBAL_SOCKET
	/* close open sockets */
	bp_closesock();
#endif

	/* remove hooks */
	initng_process_db_ptype_unregister(&parse);
#ifdef GLOBAL_SOCKET
	initng_plugin_hook_unregister(&g.FDWATCHERS, &bpf); 
	initng_plugin_hook_unregister(&g.SIGNAL, &bp_check_socket);
#endif
	initng_plugin_hook_unregister(&g.NEW_ACTIVE, &create_new_active);
	initng_plugin_hook_unregister(&g.PIPE_WATCHER, &get_pipe);
	initng_active_state_unregister(&PARSING);
	initng_active_state_unregister(&PARSE_FAIL);
#ifdef USE_LOCALEXEC
	initng_plugin_hook_unregister(&g.LAUNCH, &initng_bash_run);
#endif
}
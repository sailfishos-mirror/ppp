/*
 * main.c - Point-to-Point Protocol main module
 *
 * Copyright (c) 1984-2000 Carnegie Mellon University. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Office of Technology Transfer
 *      Carnegie Mellon University
 *      5000 Forbes Avenue
 *      Pittsburgh, PA  15213-3890
 *      (412) 268-4387, fax: (412) 268-7395
 *      tech-transfer@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Copyright (c) 1999-2024 Paul Mackerras. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THE AUTHORS OF THIS SOFTWARE DISCLAIM ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <syslog.h>
#include <netdb.h>
#include <utmp.h>
#include <pwd.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <limits.h>
#include <inttypes.h>
#include <net/if.h>

#include "pppd-private.h"
#include "options.h"
#include "magic.h"
#include "fsm.h"
#include "lcp.h"
#include "ipcp.h"
#ifdef PPP_WITH_IPV6CP
#include "ipv6cp.h"
#endif
#include "upap.h"
#include "chap.h"
#include "eap.h"
#include "ccp.h"
#include "ecp.h"
#include "pathnames.h"
#include "crypto.h"
#include "multilink.h"

#ifdef PPP_WITH_TDB
#include "tdb.h"
#endif

#ifdef PPP_WITH_CBCP
#include "cbcp.h"
#endif

#ifdef AT_CHANGE
#include "atcp.h"
#endif

/* interface vars */
char ifname[IFNAMSIZ];		/* Interface name */
int ifunit;			/* Interface unit number */

struct channel *the_channel;

char *progname;			/* Name of this program */
char hostname[MAXNAMELEN];	/* Our hostname */
static char pidfilename[MAXPATHLEN];	/* name of pid file */
static char linkpidfile[MAXPATHLEN];	/* name of linkname pid file */
uid_t uid;			/* Our real user-id */
struct notifier *pidchange = NULL;
struct notifier *phasechange = NULL;
struct notifier *exitnotify = NULL;
struct notifier *sigreceived = NULL;
struct notifier *fork_notifier = NULL;

int hungup;			/* terminal has been hung up */
int privileged;			/* we're running as real uid root */
int need_holdoff;		/* need holdoff period before restarting */
int detached;			/* have detached from terminal */
volatile int code;		/* exit status for pppd */
int unsuccess;			/* # unsuccessful connection attempts */
int do_callback;		/* != 0 if we should do callback next */
int doing_callback;		/* != 0 if we are doing callback */
int ppp_session_number;		/* Session number, for channels with such a
				   concept (eg PPPoE) */
int childwait_done;		/* have timed out waiting for children */

#ifdef PPP_WITH_TDB
TDB_CONTEXT *pppdb;		/* database for storing status etc. */
#endif

char db_key[32];

int (*holdoff_hook)(void) = NULL;
int (*new_phase_hook)(int) = NULL;
void (*snoop_recv_hook)(unsigned char *p, int len) = NULL;
void (*snoop_send_hook)(unsigned char *p, int len) = NULL;

static int conn_running;	/* we have a [dis]connector running */
static int fd_loop;		/* fd for getting demand-dial packets */

int fd_devnull;			/* fd for /dev/null */
int devfd = -1;			/* fd of underlying device */
int fd_ppp = -1;		/* fd for talking PPP */
ppp_phase_t phase;		/* where the link is at */
int kill_link;
int asked_to_quit;
int open_ccp_flag;
int listen_time;
int got_sigusr2;
int got_sigterm;
int got_sighup;

static sigset_t signals_handled;
static int waiting;
static int sigpipe[2];

char **script_env;		/* Env. variable values for scripts */
int s_env_nalloc;		/* # words avail at script_env */

u_char outpacket_buf[PPP_MRU+PPP_HDRLEN]; /* buffer for outgoing packet */
u_char inpacket_buf[PPP_MRU+PPP_HDRLEN]; /* buffer for incoming packet */

static int n_children;		/* # child processes still running */
static int got_sigchld;		/* set if we have received a SIGCHLD */

int privopen;			/* don't lock, open device as root */

char *no_ppp_msg = "Sorry - this system lacks PPP kernel support\n";

GIDSET_TYPE groups[NGROUPS_MAX];/* groups the user is in */
int ngroups;			/* How many groups valid in groups */

static struct timeval start_time;	/* Time when link was started. */

static struct pppd_stats old_link_stats;
struct pppd_stats link_stats;
unsigned link_connect_time;
int link_stats_valid;
int link_stats_print;

int error_count;

bool bundle_eof;
bool bundle_terminating;

/*
 * We maintain a list of child process pids and
 * functions to call when they exit.
 */
struct subprocess {
    pid_t	pid;
    char	*prog;
    void	(*done)(void *);
    void	*arg;
    int		killable;
    struct subprocess *next;
};

static struct subprocess *children;

/* Prototypes for procedures local to this file. */

static void setup_signals(void);
static void create_pidfile(int pid);
static void create_linkpidfile(int pid);
static void cleanup(void);
static void get_input(void);
static void calltimeout(void);
static struct timeval *timeleft(struct timeval *);
static void kill_my_pg(int);
static void hup(int);
static void term(int);
static void chld(int);
static void toggle_debug(int);
static void open_ccp(int);
static void bad_signal(int);
static void holdoff_end(void *);
static void forget_child(int pid, int status);
static int reap_kids(void);
static void childwait_end(void *);
static void run_net_script(char* script, int wait);

#ifdef PPP_WITH_TDB
static void update_db_entry(void);
static void add_db_key(const char *);
static void delete_db_key(const char *);
static void cleanup_db(void);
#endif

static void handle_events(void);
void print_link_stats(void);

extern	char	*getlogin(void);
int main(int, char *[]);

const char *ppp_hostname()
{
    return hostname;
}

bool ppp_signaled(int sig)
{
    if (sig == SIGTERM)
        return !!got_sigterm;
    if (sig == SIGUSR2)
        return !!got_sigusr2;
    if (sig == SIGHUP)
        return !!got_sighup;
    return false;
}

ppp_exit_code_t ppp_status()
{
   return code;
}

void ppp_set_status(ppp_exit_code_t value)
{
    code = value;
}

void ppp_set_session_number(int number)
{
    ppp_session_number = number;
}

int ppp_get_session_number()
{
    return ppp_session_number;
}

const char *ppp_ifname()
{
    return ifname;
}

int ppp_get_ifname(char *buf, size_t bufsz)
{
    if (buf) {
        return strlcpy(buf, ifname, bufsz);
    }
    return false;
}

void ppp_set_ifname(const char *name)
{
    if (name) {
        strlcpy(ifname, name, sizeof(ifname));
    }
}

int ppp_ifunit()
{
    return ifunit;
}

int ppp_get_link_uptime()
{
    return link_connect_time;
}

/*
 * PPP Data Link Layer "protocol" table.
 * One entry per supported protocol.
 * The last entry must be NULL.
 */
struct protent *protocols[] = {
    &lcp_protent,
    &pap_protent,
    &chap_protent,
#ifdef PPP_WITH_CBCP
    &cbcp_protent,
#endif
    &ipcp_protent,
#ifdef PPP_WITH_IPV6CP
    &ipv6cp_protent,
#endif
    &ccp_protent,
    &ecp_protent,
#ifdef AT_CHANGE
    &atcp_protent,
#endif
    &eap_protent,
    NULL
};

int
main(int argc, char *argv[])
{
    int i, t;
    char *p;
    struct passwd *pw;
    struct protent *protp;
    char numbuf[16];

    strlcpy(path_upapfile, PPP_PATH_UPAPFILE, MAXPATHLEN);
    strlcpy(path_chapfile, PPP_PATH_CHAPFILE, MAXPATHLEN);

    strlcpy(path_net_init, PPP_PATH_NET_INIT, MAXPATHLEN);
    strlcpy(path_net_preup, PPP_PATH_NET_PREUP, MAXPATHLEN);
    strlcpy(path_net_down, PPP_PATH_NET_DOWN, MAXPATHLEN);

    strlcpy(path_ipup, PPP_PATH_IPUP, MAXPATHLEN);
    strlcpy(path_ipdown, PPP_PATH_IPDOWN, MAXPATHLEN);
    strlcpy(path_ippreup, PPP_PATH_IPPREUP, MAXPATHLEN);

#ifdef PPP_WITH_IPV6CP
    strlcpy(path_ipv6up, PPP_PATH_IPV6UP, MAXPATHLEN);
    strlcpy(path_ipv6down, PPP_PATH_IPV6DOWN, MAXPATHLEN);
#endif
    link_stats_valid = 0;
    link_stats_print = 1;
    new_phase(PHASE_INITIALIZE);

    script_env = NULL;

    /* Initialize syslog facilities */
    reopen_log();

    /* Initialize crypto libraries */
    if (!PPP_crypto_init()) {
        exit(1);
    }

    if (gethostname(hostname, sizeof(hostname)) < 0 ) {
	ppp_option_error("Couldn't get hostname: %m");
	exit(1);
    }
    hostname[MAXNAMELEN-1] = 0;

    /* make sure we don't create world or group writable files. */
    umask(umask(0777) | 022);

    uid = getuid();
    privileged = uid == 0;
    slprintf(numbuf, sizeof(numbuf), "%d", uid);
    ppp_script_setenv("ORIG_UID", numbuf, 0);

    ngroups = getgroups(NGROUPS_MAX, groups);

    /*
     * Initialize magic number generator now so that protocols may
     * use magic numbers in initialization.
     */
    magic_init();

    /*
     * Initialize each protocol.
     */
    for (i = 0; (protp = protocols[i]) != NULL; ++i)
        (*protp->init)(0);

    /*
     * Initialize the default channel.
     */
    tty_init();

    progname = *argv;

    /*
     * Parse, in order, the system options file, the user's options file,
     * and the command line arguments.
     */
    if (!ppp_options_from_file(PPP_PATH_SYSOPTIONS, !privileged, 0, 1)
	|| !options_from_user()
	|| !parse_args(argc-1, argv+1))
	exit(EXIT_OPTION_ERROR);
    devnam_fixed = 1;		/* can no longer change device name */

    /*
     * Work out the device name, if it hasn't already been specified,
     * and parse the tty's options file.
     */
    if (the_channel->process_extra_options)
	(*the_channel->process_extra_options)();

    if (debug)
	setlogmask(LOG_UPTO(LOG_DEBUG));

    if (show_options) {
	showopts();
	die(0);
    }

    /*
     * Check that we are running as root.
     */
    if (geteuid() != 0) {
	ppp_option_error("must be root to run %s, since it is not setuid-root",
		     argv[0]);
	exit(EXIT_NOT_ROOT);
    }

    if (!ppp_check_kernel_support()) {
	ppp_option_error("%s", no_ppp_msg);
	exit(EXIT_NO_KERNEL_SUPPORT);
    }

    /*
     * Check that the options given are valid and consistent.
     */
    check_options();
    if (!sys_check_options())
	exit(EXIT_OPTION_ERROR);
    auth_check_options();
    mp_check_options();
    for (i = 0; (protp = protocols[i]) != NULL; ++i)
	if (protp->check_options != NULL)
	    (*protp->check_options)();
    if (the_channel->check_options)
	(*the_channel->check_options)();


    if (dump_options || dryrun) {
	init_pr_log(NULL, LOG_INFO);
	print_options(pr_log, NULL);
	end_pr_log();
    }

    if (dryrun)
	die(0);

    /* Make sure fds 0, 1, 2 are open to somewhere. */
    fd_devnull = open(PPP_DEVNULL, O_RDWR);
    if (fd_devnull < 0)
	fatal("Couldn't open %s: %m", PPP_DEVNULL);
    while (fd_devnull <= 2) {
	i = dup(fd_devnull);
	if (i < 0)
	    fatal("Critical shortage of file descriptors: dup failed: %m");
	fd_devnull = i;
    }

    event_handler_init();

    /*
     * Initialize system-dependent stuff.
     */
    sys_init();

#ifdef PPP_WITH_TDB
    pppdb = tdb_open(PPP_PATH_PPPDB, 0, 0, O_RDWR|O_CREAT, 0644);
    if (pppdb != NULL) {
	slprintf(db_key, sizeof(db_key), "pppd%d", getpid());
	update_db_entry();
    } else {
	warn("Warning: couldn't open ppp database %s", PPP_PATH_PPPDB);
	if (multilink) {
	    warn("Warning: disabling multilink");
	    multilink = 0;
	}
    }
#endif

    /*
     * Detach ourselves from the terminal, if required,
     * and identify who is running us.
     */
    if (!nodetach && !updetach)
	detach();
    p = getlogin();
    if (p == NULL) {
	pw = getpwuid(uid);
	if (pw != NULL && pw->pw_name != NULL)
	    p = pw->pw_name;
	else
	    p = "(unknown)";
    }
    syslog(LOG_NOTICE, "pppd %s started by %s, uid %d", VERSION, p, uid);
    ppp_script_setenv("PPPLOGNAME", p, 0);

    if (devnam[0])
	ppp_script_setenv("DEVICE", devnam, 1);
    slprintf(numbuf, sizeof(numbuf), "%d", getpid());
    ppp_script_setenv("PPPD_PID", numbuf, 1);

    setup_signals();

    create_linkpidfile(getpid());

    waiting = 0;

    /*
     * If we're doing dial-on-demand, set up the interface now.
     */
    if (demand) {
	/*
	 * Open the loopback channel and set it up to be the ppp interface.
	 */
	fd_loop = open_ppp_loopback();
	set_ifunit(1);
	/*
	 * Configure the interface and mark it up, etc.
	 */
	demand_conf();
    }

    do_callback = 0;
    for (;;) {

	bundle_eof = 0;
	bundle_terminating = 0;
	listen_time = 0;
	need_holdoff = 1;
	devfd = -1;
	code = EXIT_OK;
	++unsuccess;
	doing_callback = do_callback;
	do_callback = 0;

	if (demand && !doing_callback) {
	    /*
	     * Don't do anything until we see some activity.
	     */
	    new_phase(PHASE_DORMANT);
	    demand_unblock();
	    add_fd(fd_loop);
	    for (;;) {
		handle_events();
		if (asked_to_quit)
		    break;
		if (get_loop_output())
		    break;
	    }
	    remove_fd(fd_loop);
	    if (asked_to_quit)
		break;

	    /*
	     * Now we want to bring up the link.
	     */
	    demand_block();
	    info("Starting link");
	}

	ppp_get_time(&start_time);
	ppp_script_unsetenv("CONNECT_TIME");
	ppp_script_unsetenv("BYTES_SENT");
	ppp_script_unsetenv("BYTES_RCVD");

	lcp_open(0);		/* Start protocol */
	start_link(0);
	while (phase != PHASE_DEAD) {
	    handle_events();
	    get_input();
	    if (kill_link) {
		lcp_close(0, "User request");
		need_holdoff = 0;
	    }
	    if (asked_to_quit) {
		bundle_terminating = 1;
		if (phase == PHASE_MASTER)
		    mp_bundle_terminated();
	    }
	    if (open_ccp_flag) {
		if (phase == PHASE_NETWORK || phase == PHASE_RUNNING) {
		    ccp_fsm[0].flags = OPT_RESTART; /* clears OPT_SILENT */
		    (*ccp_protent.open)(0);
		}
	    }
	}
	/* restore FSMs to original state */
	lcp_close(0, "");

	if (!persist || asked_to_quit || (maxfail > 0 && unsuccess >= maxfail))
	    break;

	if (demand)
	    demand_discard();
	t = need_holdoff? holdoff: 0;
	if (holdoff_hook)
	    t = (*holdoff_hook)();
	if (t > 0) {
	    new_phase(PHASE_HOLDOFF);
	    TIMEOUT(holdoff_end, NULL, t);
	    do {
		handle_events();
		if (kill_link)
		    new_phase(PHASE_DORMANT); /* allow signal to end holdoff */
	    } while (phase == PHASE_HOLDOFF);
	    if (!persist)
		break;
	}
    }

    /* Wait for scripts to finish */
    reap_kids();
    if (n_children > 0) {
	if (child_wait > 0)
	    TIMEOUT(childwait_end, NULL, child_wait);
	if (debug) {
	    struct subprocess *chp;
	    dbglog("Waiting for %d child processes...", n_children);
	    for (chp = children; chp != NULL; chp = chp->next)
		dbglog("  script %s, pid %d", chp->prog, chp->pid);
	}
	while (n_children > 0 && !childwait_done) {
	    handle_events();
	    if (kill_link && !childwait_done)
		childwait_end(NULL);
	}
    }

    PPP_crypto_deinit();
    die(code);
    return 0;
}

/*
 * handle_events - wait for something to happen and respond to it.
 */
static void
handle_events(void)
{
    struct timeval timo;
    unsigned char buf[16];

    kill_link = open_ccp_flag = 0;

    /* alert via signal pipe */
    waiting = 1;
    /* flush signal pipe */
    for (; read(sigpipe[0], buf, sizeof(buf)) > 0; );
    add_fd(sigpipe[0]);
    /* wait if necessary */
    if (!(got_sighup || got_sigterm || got_sigusr2 || got_sigchld))
	wait_input(timeleft(&timo));
    waiting = 0;
    remove_fd(sigpipe[0]);

    calltimeout();
    if (got_sighup) {
	info("Hangup (SIGHUP)");
	kill_link = 1;
	got_sighup = 0;
	if (code != EXIT_HANGUP)
	    code = EXIT_USER_REQUEST;
    }
    if (got_sigterm) {
	info("Terminating on signal %d", got_sigterm);
	kill_link = 1;
	asked_to_quit = 1;
	persist = 0;
	code = EXIT_USER_REQUEST;
	got_sigterm = 0;
    }
    if (got_sigchld) {
	got_sigchld = 0;
	reap_kids();	/* Don't leave dead kids lying around */
    }
    if (got_sigusr2) {
	open_ccp_flag = 1;
	got_sigusr2 = 0;
    }
}

/*
 * setup_signals - initialize signal handling.
 */
static void
setup_signals(void)
{
    struct sigaction sa;

    /* create pipe to wake up event handler from signal handler */
    if (pipe(sigpipe) < 0)
	fatal("Couldn't create signal pipe: %m");
    fcntl(sigpipe[0], F_SETFD, fcntl(sigpipe[0], F_GETFD) | FD_CLOEXEC);
    fcntl(sigpipe[1], F_SETFD, fcntl(sigpipe[1], F_GETFD) | FD_CLOEXEC);
    fcntl(sigpipe[0], F_SETFL, fcntl(sigpipe[0], F_GETFL) | O_NONBLOCK);
    fcntl(sigpipe[1], F_SETFL, fcntl(sigpipe[1], F_GETFL) | O_NONBLOCK);

    /*
     * Compute mask of all interesting signals and install signal handlers
     * for each.  Only one signal handler may be active at a time.  Therefore,
     * all other signals should be masked when any handler is executing.
     */
    sigemptyset(&signals_handled);
    sigaddset(&signals_handled, SIGHUP);
    sigaddset(&signals_handled, SIGINT);
    sigaddset(&signals_handled, SIGTERM);
    sigaddset(&signals_handled, SIGCHLD);
    sigaddset(&signals_handled, SIGUSR2);

#define SIGNAL(s, handler)	do { \
	sa.sa_handler = handler; \
	if (sigaction(s, &sa, NULL) < 0) \
	    fatal("Couldn't establish signal handler (%d): %m", s); \
    } while (0)

    sa.sa_mask = signals_handled;
    sa.sa_flags = 0;
    SIGNAL(SIGHUP, hup);		/* Hangup */
    SIGNAL(SIGINT, term);		/* Interrupt */
    SIGNAL(SIGTERM, term);		/* Terminate */
    SIGNAL(SIGCHLD, chld);

    SIGNAL(SIGUSR1, toggle_debug);	/* Toggle debug flag */
    SIGNAL(SIGUSR2, open_ccp);		/* Reopen CCP */

    /*
     * Install a handler for other signals which would otherwise
     * cause pppd to exit without cleaning up.
     */
    SIGNAL(SIGABRT, bad_signal);
    SIGNAL(SIGALRM, bad_signal);
    SIGNAL(SIGFPE, bad_signal);
    SIGNAL(SIGILL, bad_signal);
    SIGNAL(SIGPIPE, bad_signal);
    SIGNAL(SIGQUIT, bad_signal);
    SIGNAL(SIGSEGV, bad_signal);
#ifdef SIGBUS
    SIGNAL(SIGBUS, bad_signal);
#endif
#ifdef SIGEMT
    SIGNAL(SIGEMT, bad_signal);
#endif
#ifdef SIGPOLL
    SIGNAL(SIGPOLL, bad_signal);
#endif
#ifdef SIGPROF
    SIGNAL(SIGPROF, bad_signal);
#endif
#ifdef SIGSYS
    SIGNAL(SIGSYS, bad_signal);
#endif
#ifdef SIGTRAP
    SIGNAL(SIGTRAP, bad_signal);
#endif
#ifdef SIGVTALRM
    SIGNAL(SIGVTALRM, bad_signal);
#endif
#ifdef SIGXCPU
    SIGNAL(SIGXCPU, bad_signal);
#endif
#ifdef SIGXFSZ
    SIGNAL(SIGXFSZ, bad_signal);
#endif

    /*
     * Apparently we can get a SIGPIPE when we call syslog, if
     * syslogd has died and been restarted.  Ignoring it seems
     * be sufficient.
     */
    signal(SIGPIPE, SIG_IGN);
}

/*
 * net-* scripts to be run come through here.
 */
void run_net_script(char* script, int wait)
{
    char strspeed[32];
    char *argv[6];

    slprintf(strspeed, sizeof(strspeed), "%d", baud_rate);

    argv[0] = script;
    argv[1] = ifname;
    argv[2] = devnam;
    argv[3] = strspeed;
    argv[4] = ipparam;
    argv[5] = NULL;

    run_program(script, argv, 0, NULL, NULL, wait);
}

/*
 * set_ifunit - do things we need to do once we know which ppp
 * unit we are using.
 */
void
set_ifunit(int iskey)
{
    char ifkey[32];

    if (req_ifname[0] != '\0')
	slprintf(ifname, sizeof(ifname), "%s", req_ifname);
    else
	slprintf(ifname, sizeof(ifname), "%s%d", PPP_DRV_NAME, ifunit);
    info("Using interface %s", ifname);
    ppp_script_setenv("IFNAME", ifname, iskey);
    slprintf(ifkey, sizeof(ifkey), "%d", ifunit);
    ppp_script_setenv("UNIT", ifkey, iskey);
    if (iskey) {
	create_pidfile(getpid());	/* write pid to file */
	create_linkpidfile(getpid());
    }
    if (*remote_number)
        ppp_script_setenv("REMOTENUMBER", remote_number, 0);
    run_net_script(path_net_init, 1);
}

/*
 * detach - detach us from the controlling terminal.
 */
void
detach(void)
{
    int pid;
    int ret;
    char numbuf[16];
    int pipefd[2];

    if (detached)
	return;
    if (pipe(pipefd) == -1)
	pipefd[0] = pipefd[1] = -1;
    if ((pid = fork()) < 0) {
	error("Couldn't detach (fork failed: %m)");
	die(1);			/* or just return? */
    }
    if (pid != 0) {
	/* parent */
	notify(pidchange, pid);
	/* update pid files if they have been written already */
	if (pidfilename[0])
	    create_pidfile(pid);
	create_linkpidfile(pid);
	exit(0);		/* parent dies */
    }
    setsid();
    ret = chdir("/");
    if (ret != 0) {
        fatal("Could not change directory to '/', %m");
    }
    dup2(fd_devnull, 0);
    dup2(fd_devnull, 1);
    dup2(fd_devnull, 2);
    detached = 1;
    if (log_default)
	log_to_fd = -1;
    slprintf(numbuf, sizeof(numbuf), "%d", getpid());
    ppp_script_setenv("PPPD_PID", numbuf, 1);

    /* wait for parent to finish updating pid & lock files and die */
    close(pipefd[1]);
    complete_read(pipefd[0], numbuf, 1);
    close(pipefd[0]);
}

/*
 * reopen_log - (re)open our connection to syslog.
 */
void
reopen_log(void)
{
    openlog("pppd", LOG_PID | LOG_NDELAY, LOG_PPP);
    setlogmask(LOG_UPTO(LOG_INFO));
}

/*
 * Create a file containing our process ID.
 */
static void
create_pidfile(int pid)
{
    FILE *pidfile;

    mkdir_recursive(PPP_PATH_VARRUN);
    slprintf(pidfilename, sizeof(pidfilename), "%s/%s.pid",
	     PPP_PATH_VARRUN, ifname);
    if ((pidfile = fopen(pidfilename, "w")) != NULL) {
	fprintf(pidfile, "%d\n", pid);
	(void) fclose(pidfile);
    } else {
	error("Failed to create pid file %s: %m", pidfilename);
	pidfilename[0] = 0;
    }
}

void
create_linkpidfile(int pid)
{
    FILE *pidfile;

    if (linkname[0] == 0)
	return;
    ppp_script_setenv("LINKNAME", linkname, 1);
    slprintf(linkpidfile, sizeof(linkpidfile), "%s/ppp-%s.pid",
	     PPP_PATH_VARRUN, linkname);
    if ((pidfile = fopen(linkpidfile, "w")) != NULL) {
	fprintf(pidfile, "%d\n", pid);
	if (ifname[0])
	    fprintf(pidfile, "%s\n", ifname);
	(void) fclose(pidfile);
    } else {
	error("Failed to create pid file %s: %m", linkpidfile);
	linkpidfile[0] = 0;
    }
}

/*
 * remove_pidfile - remove our pid files
 */
void remove_pidfiles(void)
{
    if (pidfilename[0] != 0 && unlink(pidfilename) < 0 && errno != ENOENT)
	warn("unable to delete pid file %s: %m", pidfilename);
    pidfilename[0] = 0;
    if (linkpidfile[0] != 0 && unlink(linkpidfile) < 0 && errno != ENOENT)
	warn("unable to delete pid file %s: %m", linkpidfile);
    linkpidfile[0] = 0;
}

/*
 * holdoff_end - called via a timeout when the holdoff period ends.
 */
static void
holdoff_end(void *arg)
{
    new_phase(PHASE_DORMANT);
}

/* List of protocol names, to make our messages a little more informative. */
struct protocol_list {
    u_short	proto;
    const char	*name;
} protocol_list[] = {
    { 0x21,	"IP" },
    { 0x23,	"OSI Network Layer" },
    { 0x25,	"Xerox NS IDP" },
    { 0x27,	"DECnet Phase IV" },
    { 0x29,	"Appletalk" },
    { 0x2b,	"Novell IPX" },
    { 0x2d,	"VJ compressed TCP/IP" },
    { 0x2f,	"VJ uncompressed TCP/IP" },
    { 0x31,	"Bridging PDU" },
    { 0x33,	"Stream Protocol ST-II" },
    { 0x35,	"Banyan Vines" },
    { 0x39,	"AppleTalk EDDP" },
    { 0x3b,	"AppleTalk SmartBuffered" },
    { 0x3d,	"Multi-Link" },
    { 0x3f,	"NETBIOS Framing" },
    { 0x41,	"Cisco Systems" },
    { 0x43,	"Ascom Timeplex" },
    { 0x45,	"Fujitsu Link Backup and Load Balancing (LBLB)" },
    { 0x47,	"DCA Remote Lan" },
    { 0x49,	"Serial Data Transport Protocol (PPP-SDTP)" },
    { 0x4b,	"SNA over 802.2" },
    { 0x4d,	"SNA" },
    { 0x4f,	"IP6 Header Compression" },
    { 0x51,	"KNX Bridging Data" },
    { 0x53,	"Encryption" },
    { 0x55,	"Individual Link Encryption" },
    { 0x57,	"IPv6" },
    { 0x59,	"PPP Muxing" },
    { 0x5b,	"Vendor-Specific Network Protocol" },
    { 0x61,	"RTP IPHC Full Header" },
    { 0x63,	"RTP IPHC Compressed TCP" },
    { 0x65,	"RTP IPHC Compressed non-TCP" },
    { 0x67,	"RTP IPHC Compressed UDP 8" },
    { 0x69,	"RTP IPHC Compressed RTP 8" },
    { 0x6f,	"Stampede Bridging" },
    { 0x73,	"MP+" },
    { 0xc1,	"NTCITS IPI" },
    { 0xfb,	"single-link compression" },
    { 0xfd,	"Compressed Datagram" },
    { 0x0201,	"802.1d Hello Packets" },
    { 0x0203,	"IBM Source Routing BPDU" },
    { 0x0205,	"DEC LANBridge100 Spanning Tree" },
    { 0x0207,	"Cisco Discovery Protocol" },
    { 0x0209,	"Netcs Twin Routing" },
    { 0x020b,	"STP - Scheduled Transfer Protocol" },
    { 0x020d,	"EDP - Extreme Discovery Protocol" },
    { 0x0211,	"Optical Supervisory Channel Protocol" },
    { 0x0213,	"Optical Supervisory Channel Protocol" },
    { 0x0231,	"Luxcom" },
    { 0x0233,	"Sigma Network Systems" },
    { 0x0235,	"Apple Client Server Protocol" },
    { 0x0281,	"MPLS Unicast" },
    { 0x0283,	"MPLS Multicast" },
    { 0x0285,	"IEEE p1284.4 standard - data packets" },
    { 0x0287,	"ETSI TETRA Network Protocol Type 1" },
    { 0x0289,	"Multichannel Flow Treatment Protocol" },
    { 0x2063,	"RTP IPHC Compressed TCP No Delta" },
    { 0x2065,	"RTP IPHC Context State" },
    { 0x2067,	"RTP IPHC Compressed UDP 16" },
    { 0x2069,	"RTP IPHC Compressed RTP 16" },
    { 0x4001,	"Cray Communications Control Protocol" },
    { 0x4003,	"CDPD Mobile Network Registration Protocol" },
    { 0x4005,	"Expand accelerator protocol" },
    { 0x4007,	"ODSICP NCP" },
    { 0x4009,	"DOCSIS DLL" },
    { 0x400B,	"Cetacean Network Detection Protocol" },
    { 0x4021,	"Stacker LZS" },
    { 0x4023,	"RefTek Protocol" },
    { 0x4025,	"Fibre Channel" },
    { 0x4027,	"EMIT Protocols" },
    { 0x405b,	"Vendor-Specific Protocol (VSP)" },
    { 0x8021,	"Internet Protocol Control Protocol" },
    { 0x8023,	"OSI Network Layer Control Protocol" },
    { 0x8025,	"Xerox NS IDP Control Protocol" },
    { 0x8027,	"DECnet Phase IV Control Protocol" },
    { 0x8029,	"Appletalk Control Protocol" },
    { 0x802b,	"Novell IPX Control Protocol" },
    { 0x8031,	"Bridging NCP" },
    { 0x8033,	"Stream Protocol Control Protocol" },
    { 0x8035,	"Banyan Vines Control Protocol" },
    { 0x803d,	"Multi-Link Control Protocol" },
    { 0x803f,	"NETBIOS Framing Control Protocol" },
    { 0x8041,	"Cisco Systems Control Protocol" },
    { 0x8043,	"Ascom Timeplex" },
    { 0x8045,	"Fujitsu LBLB Control Protocol" },
    { 0x8047,	"DCA Remote Lan Network Control Protocol (RLNCP)" },
    { 0x8049,	"Serial Data Control Protocol (PPP-SDCP)" },
    { 0x804b,	"SNA over 802.2 Control Protocol" },
    { 0x804d,	"SNA Control Protocol" },
    { 0x804f,	"IP6 Header Compression Control Protocol" },
    { 0x8051,	"KNX Bridging Control Protocol" },
    { 0x8053,	"Encryption Control Protocol" },
    { 0x8055,	"Individual Link Encryption Control Protocol" },
    { 0x8057,	"IPv6 Control Protocol" },
    { 0x8059,	"PPP Muxing Control Protocol" },
    { 0x805b,	"Vendor-Specific Network Control Protocol (VSNCP)" },
    { 0x806f,	"Stampede Bridging Control Protocol" },
    { 0x8073,	"MP+ Control Protocol" },
    { 0x80c1,	"NTCITS IPI Control Protocol" },
    { 0x80fb,	"Single Link Compression Control Protocol" },
    { 0x80fd,	"Compression Control Protocol" },
    { 0x8207,	"Cisco Discovery Protocol Control" },
    { 0x8209,	"Netcs Twin Routing" },
    { 0x820b,	"STP - Control Protocol" },
    { 0x820d,	"EDPCP - Extreme Discovery Protocol Ctrl Prtcl" },
    { 0x8235,	"Apple Client Server Protocol Control" },
    { 0x8281,	"MPLSCP" },
    { 0x8285,	"IEEE p1284.4 standard - Protocol Control" },
    { 0x8287,	"ETSI TETRA TNP1 Control Protocol" },
    { 0x8289,	"Multichannel Flow Treatment Protocol" },
    { 0xc021,	"Link Control Protocol" },
    { 0xc023,	"Password Authentication Protocol" },
    { 0xc025,	"Link Quality Report" },
    { 0xc027,	"Shiva Password Authentication Protocol" },
    { 0xc029,	"CallBack Control Protocol (CBCP)" },
    { 0xc02b,	"BACP Bandwidth Allocation Control Protocol" },
    { 0xc02d,	"BAP" },
    { 0xc05b,	"Vendor-Specific Authentication Protocol (VSAP)" },
    { 0xc081,	"Container Control Protocol" },
    { 0xc223,	"Challenge Handshake Authentication Protocol" },
    { 0xc225,	"RSA Authentication Protocol" },
    { 0xc227,	"Extensible Authentication Protocol" },
    { 0xc229,	"Mitsubishi Security Info Exch Ptcl (SIEP)" },
    { 0xc26f,	"Stampede Bridging Authorization Protocol" },
    { 0xc281,	"Proprietary Authentication Protocol" },
    { 0xc283,	"Proprietary Authentication Protocol" },
    { 0xc481,	"Proprietary Node ID Authentication Protocol" },
    { 0,	NULL },
};

/*
 * protocol_name - find a name for a PPP protocol.
 */
const char *
protocol_name(int proto)
{
    struct protocol_list *lp;

    for (lp = protocol_list; lp->proto != 0; ++lp)
	if (proto == lp->proto)
	    return lp->name;
    return NULL;
}

/*
 * get_input - called when incoming data is available.
 */
static void
get_input(void)
{
    int len, i;
    u_char *p;
    u_short protocol;
    struct protent *protp;

    p = inpacket_buf;	/* point to beginning of packet buffer */

    len = read_packet(inpacket_buf);
    if (len < 0)
	return;

    if (len == 0) {
	if (bundle_eof && mp_master()) {
	    notice("Last channel has disconnected");
	    mp_bundle_terminated();
	    return;
	}
	notice("Modem hangup");
	hungup = 1;
	code = EXIT_HANGUP;
	need_holdoff = 0;
	lcp_lowerdown(0);	/* serial link is no longer available */
	link_terminated(0);
	return;
    }

    if (len < PPP_HDRLEN) {
	dbglog("received short packet:%.*B", len, p);
	return;
    }

    dump_packet("rcvd", p, len);
    if (snoop_recv_hook) snoop_recv_hook(p, len);

    p += 2;				/* Skip address and control */
    GETSHORT(protocol, p);
    len -= PPP_HDRLEN;

    /*
     * Toss all non-LCP packets unless LCP is OPEN.
     */
    if (protocol != PPP_LCP && lcp_fsm[0].state != OPENED) {
	dbglog("Discarded non-LCP packet when LCP not open");
	return;
    }

    /*
     * Until we get past the authentication phase, toss all packets
     * except LCP, LQR and authentication packets.
     */
    if (phase <= PHASE_AUTHENTICATE
	&& !(protocol == PPP_LCP || protocol == PPP_LQR
	     || protocol == PPP_PAP || protocol == PPP_CHAP ||
		protocol == PPP_EAP)) {
	dbglog("discarding proto 0x%x in phase %d",
		   protocol, phase);
	return;
    }

    /*
     * Upcall the proper protocol input routine.
     */
    for (i = 0; (protp = protocols[i]) != NULL; ++i) {
	if (protp->protocol == protocol && protp->enabled_flag) {
	    (*protp->input)(0, p, len);
	    return;
	}
        if (protocol == (protp->protocol & ~0x8000) && protp->enabled_flag
	    && protp->datainput != NULL) {
	    (*protp->datainput)(0, p, len);
	    return;
	}
    }

    if (debug) {
	const char *pname = protocol_name(protocol);
	if (pname != NULL)
	    warn("Unsupported protocol '%s' (0x%x) received", pname, protocol);
	else
	    warn("Unsupported protocol 0x%x received", protocol);
    }
    lcp_sprotrej(0, p - PPP_HDRLEN, len + PPP_HDRLEN);
}

/*
 * ppp_send_config - configure the transmit-side characteristics of
 * the ppp interface.  Returns -1, indicating an error, if the channel
 * send_config procedure called error() (or incremented error_count
 * itself), otherwise 0.
 */
int
ppp_send_config(int unit, int mtu, u_int32_t accm, int pcomp, int accomp)
{
	int errs;

	if (the_channel->send_config == NULL)
		return 0;
	errs = error_count;
	(*the_channel->send_config)(mtu, accm, pcomp, accomp);
	return (error_count != errs)? -1: 0;
}

/*
 * ppp_recv_config - configure the receive-side characteristics of
 * the ppp interface.  Returns -1, indicating an error, if the channel
 * recv_config procedure called error() (or incremented error_count
 * itself), otherwise 0.
 */
int
ppp_recv_config(int unit, int mru, u_int32_t accm, int pcomp, int accomp)
{
	int errs;

	if (the_channel->recv_config == NULL)
		return 0;
	errs = error_count;
	(*the_channel->recv_config)(mru, accm, pcomp, accomp);
	return (error_count != errs)? -1: 0;
}

/*
 * new_phase - signal the start of a new phase of pppd's operation.
 */
void
new_phase(ppp_phase_t p)
{
    switch (p) {
    case PHASE_NETWORK:
	if (phase <= PHASE_NETWORK) {
	    char iftmpname[IFNAMSIZ];
	    int ifindex = if_nametoindex(ifname);
	    run_net_script(path_net_preup, 1);
	    if (if_indextoname(ifindex, iftmpname) && strcmp(iftmpname, ifname)) {
		info("Detected interface name change from %s to %s.", ifname, iftmpname);
		strcpy(ifname, iftmpname);
	    }
	}
	break;
    case PHASE_DISCONNECT:
	run_net_script(path_net_down, 0);
	break;
    }

    phase = p;
    if (new_phase_hook)
	(*new_phase_hook)(p);
    notify(phasechange, p);
}

bool
in_phase(ppp_phase_t p)
{
    return (phase == p);
}

/*
 * die - clean up state and exit with the specified status.
 */
void
die(int status)
{

    if (!mp_on() || mp_master())
	print_link_stats();
    cleanup();
    notify(exitnotify, status);
    syslog(LOG_INFO, "Exit.");
    exit(status);
}

/*
 * cleanup - restore anything which needs to be restored before we exit
 */
/* ARGSUSED */
static void
cleanup(void)
{
    sys_cleanup();

    if (fd_ppp >= 0)
	the_channel->disestablish_ppp(devfd);
    if (the_channel->cleanup)
	(*the_channel->cleanup)();
    remove_pidfiles();

#ifdef PPP_WITH_TDB
    if (pppdb != NULL)
	cleanup_db();
#endif

}

void
print_link_stats(void)
{
    /*
     * Print connect time and statistics.
     */
    if (link_stats_print && link_stats_valid) {
       int t = (link_connect_time + 5) / 6;    /* 1/10ths of minutes */
       info("Connect time %d.%d minutes.", t/10, t%10);
       info("Sent %llu bytes, received %llu bytes.",
	    link_stats.bytes_out, link_stats.bytes_in);
       link_stats_print = 0;
    }
}

/*
 * reset_link_stats - "reset" stats when link goes up.
 */
void
reset_link_stats(int u)
{
    get_ppp_stats(u, &old_link_stats);
    ppp_get_time(&start_time);
    link_stats_print = 1;
}

/*
 * update_link_stats - get stats at link termination.
 */
void
update_link_stats(int u)
{
    struct timeval now;
    char numbuf[32];

    if (!get_ppp_stats(u, &link_stats)
	|| ppp_get_time(&now) < 0)
	return;
    link_connect_time = now.tv_sec - start_time.tv_sec;
    link_stats_valid = 1;

    link_stats.bytes_in  -= old_link_stats.bytes_in;
    link_stats.bytes_out -= old_link_stats.bytes_out;
    link_stats.pkts_in   -= old_link_stats.pkts_in;
    link_stats.pkts_out  -= old_link_stats.pkts_out;

    slprintf(numbuf, sizeof(numbuf), "%u", link_connect_time);
    ppp_script_setenv("CONNECT_TIME", numbuf, 0);
    snprintf(numbuf, sizeof(numbuf), "%" PRIu64, link_stats.bytes_out);
    ppp_script_setenv("BYTES_SENT", numbuf, 0);
    snprintf(numbuf, sizeof(numbuf), "%" PRIu64, link_stats.bytes_in);
    ppp_script_setenv("BYTES_RCVD", numbuf, 0);
}

bool
ppp_get_link_stats(ppp_link_stats_st *stats)
{
    update_link_stats(0);
    if (stats != NULL &&
        link_stats_valid) {

        memcpy(stats, &link_stats, sizeof(*stats));
        return true;
    }
    return false;
}


struct	callout {
    struct timeval	c_time;		/* time at which to call routine */
    void		*c_arg;		/* argument to routine */
    void		(*c_func)(void *); /* routine */
    struct		callout *c_next;
};

static struct callout *callout = NULL;	/* Callout list */
static struct timeval timenow;		/* Current time */

/*
 * timeout - Schedule a timeout.
 */
void
ppp_timeout(void (*func)(void *), void *arg, int secs, int usecs)
{
    struct callout *newp, *p, **pp;

    /*
     * Allocate timeout.
     */
    if ((newp = (struct callout *) malloc(sizeof(struct callout))) == NULL)
	fatal("Out of memory in timeout()!");
    newp->c_arg = arg;
    newp->c_func = func;
    ppp_get_time(&timenow);
    newp->c_time.tv_sec = timenow.tv_sec + secs;
    newp->c_time.tv_usec = timenow.tv_usec + usecs;
    if (newp->c_time.tv_usec >= 1000000) {
	newp->c_time.tv_sec += newp->c_time.tv_usec / 1000000;
	newp->c_time.tv_usec %= 1000000;
    }

    /*
     * Find correct place and link it in.
     */
    for (pp = &callout; (p = *pp); pp = &p->c_next)
	if (newp->c_time.tv_sec < p->c_time.tv_sec
	    || (newp->c_time.tv_sec == p->c_time.tv_sec
		&& newp->c_time.tv_usec < p->c_time.tv_usec))
	    break;
    newp->c_next = p;
    *pp = newp;
}


/*
 * untimeout - Unschedule a timeout.
 */
void
ppp_untimeout(void (*func)(void *), void *arg)
{
    struct callout **copp, *freep;

    /*
     * Find first matching timeout and remove it from the list.
     */
    for (copp = &callout; (freep = *copp); copp = &freep->c_next)
	if (freep->c_func == func && freep->c_arg == arg) {
	    *copp = freep->c_next;
	    free((char *) freep);
	    break;
	}
}


/*
 * calltimeout - Call any timeout routines which are now due.
 */
static void
calltimeout(void)
{
    struct callout *p;

    while (callout != NULL) {
	p = callout;

	if (ppp_get_time(&timenow) < 0)
	    fatal("Failed to get time of day: %m");
	if (!(p->c_time.tv_sec < timenow.tv_sec
	      || (p->c_time.tv_sec == timenow.tv_sec
		  && p->c_time.tv_usec <= timenow.tv_usec)))
	    break;		/* no, it's not time yet */

	callout = p->c_next;
	(*p->c_func)(p->c_arg);

	free((char *) p);
    }
}


/*
 * timeleft - return the length of time until the next timeout is due.
 */
static struct timeval *
timeleft(struct timeval *tvp)
{
    if (callout == NULL)
	return NULL;

    ppp_get_time(&timenow);
    tvp->tv_sec = callout->c_time.tv_sec - timenow.tv_sec;
    tvp->tv_usec = callout->c_time.tv_usec - timenow.tv_usec;
    if (tvp->tv_usec < 0) {
	tvp->tv_usec += 1000000;
	tvp->tv_sec -= 1;
    }
    if (tvp->tv_sec < 0)
	tvp->tv_sec = tvp->tv_usec = 0;

    return tvp;
}


/*
 * kill_my_pg - send a signal to our process group, and ignore it ourselves.
 * We assume that sig is currently blocked.
 */
static void
kill_my_pg(int sig)
{
    struct sigaction act, oldact;
    struct subprocess *chp;

    if (!detached) {
	/*
	 * There might be other things in our process group that we
	 * didn't start that would get hit if we did a kill(0), so
	 * just send the signal individually to our children.
	 */
	for (chp = children; chp != NULL; chp = chp->next)
	    if (chp->killable)
		kill(chp->pid, sig);
	return;
    }

    /* We've done a setsid(), so we can just use a kill(0) */
    sigemptyset(&act.sa_mask);		/* unnecessary in fact */
    act.sa_handler = SIG_IGN;
    act.sa_flags = 0;
    kill(0, sig);
    /*
     * The kill() above made the signal pending for us, as well as
     * the rest of our process group, but we don't want it delivered
     * to us.  It is blocked at the moment.  Setting it to be ignored
     * will cause the pending signal to be discarded.  If we did the
     * kill() after setting the signal to be ignored, it is unspecified
     * (by POSIX) whether the signal is immediately discarded or left
     * pending, and in fact Linux would leave it pending, and so it
     * would be delivered after the current signal handler exits,
     * leading to an infinite loop.
     */
    sigaction(sig, &act, &oldact);
    sigaction(sig, &oldact, NULL);
}


/*
 * hup - Catch SIGHUP signal.
 *
 * Indicates that the physical layer has been disconnected.
 * We don't rely on this indication; if the user has sent this
 * signal, we just take the link down.
 */
static void
hup(int sig)
{
    /* can't log a message here, it can deadlock */
    got_sighup = 1;
    if (conn_running)
	/* Send the signal to the [dis]connector process(es) also */
	kill_my_pg(sig);
    notify(sigreceived, sig);
    if (waiting) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
	write(sigpipe[1], &sig, sizeof(sig));
#pragma GCC diagnostic pop
    }
}


/*
 * term - Catch SIGTERM signal and SIGINT signal (^C/del).
 *
 * Indicates that we should initiate a graceful disconnect and exit.
 */
/*ARGSUSED*/
static void
term(int sig)
{
    /* can't log a message here, it can deadlock */
    got_sigterm = sig;
    if (conn_running)
	/* Send the signal to the [dis]connector process(es) also */
	kill_my_pg(sig);
    notify(sigreceived, sig);
    if (waiting) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
	write(sigpipe[1], &sig, sizeof(sig));
#pragma GCC diagnostic pop
    }
}


/*
 * chld - Catch SIGCHLD signal.
 * Sets a flag so we will call reap_kids in the mainline.
 */
static void
chld(int sig)
{
    got_sigchld = 1;
    if (waiting) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
	write(sigpipe[1], &sig, sizeof(sig));
#pragma GCC diagnostic pop
    }
}


/*
 * toggle_debug - Catch SIGUSR1 signal.
 *
 * Toggle debug flag.
 */
/*ARGSUSED*/
static void
toggle_debug(int sig)
{
    debug = !debug;
    if (debug) {
	setlogmask(LOG_UPTO(LOG_DEBUG));
    } else {
	setlogmask(LOG_UPTO(LOG_WARNING));
    }
}


/*
 * open_ccp - Catch SIGUSR2 signal.
 *
 * Try to (re)negotiate compression.
 */
/*ARGSUSED*/
static void
open_ccp(int sig)
{
    got_sigusr2 = 1;
    if (waiting) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
	write(sigpipe[1], &sig, sizeof(sig));
#pragma GCC diagnostic pop
    }
}


/*
 * bad_signal - We've caught a fatal signal.  Clean up state and exit.
 */
static void
bad_signal(int sig)
{
    static int crashed = 0;

    if (crashed)
	_exit(127);
    crashed = 1;
    error("Fatal signal %d", sig);
    if (conn_running)
	kill_my_pg(SIGTERM);
    notify(sigreceived, sig);
    die(127);
}

/*
 * ppp_safe_fork - Create a child process.  The child closes all the
 * file descriptors that we don't want to leak to a script.
 * The parent waits for the child to do this before returning.
 * This also arranges for the specified fds to be dup'd to
 * fds 0, 1, 2 in the child.
 */
pid_t
ppp_safe_fork(int infd, int outfd, int errfd)
{
	pid_t pid;
	int fd, pipefd[2];
	char buf[1];

	/* make sure fds 0, 1, 2 are occupied (probably not necessary) */
	while ((fd = dup(fd_devnull)) >= 0) {
		if (fd > 2) {
			close(fd);
			break;
		}
	}

	if (pipe(pipefd) == -1)
		pipefd[0] = pipefd[1] = -1;
	pid = fork();
	if (pid < 0) {
		error("fork failed: %m");
		return -1;
	}
	if (pid > 0) {
		/* parent */
		close(pipefd[1]);
		/* this read() blocks until the close(pipefd[1]) below */
		complete_read(pipefd[0], buf, 1);
		close(pipefd[0]);
		return pid;
	}

	/* Executing in the child */
	ppp_sys_close();
#ifdef PPP_WITH_TDB
	if (pppdb != NULL)
		tdb_close(pppdb);
#endif

	/* make sure infd, outfd and errfd won't get tromped on below */
	if (infd == 1 || infd == 2)
		infd = dup(infd);
	if (outfd == 0 || outfd == 2)
		outfd = dup(outfd);
	if (errfd == 0 || errfd == 1)
		errfd = dup(errfd);

	closelog();

	/* dup the in, out, err fds to 0, 1, 2 */
	if (infd != 0)
		dup2(infd, 0);
	if (outfd != 1)
		dup2(outfd, 1);
	if (errfd != 2)
		dup2(errfd, 2);

	if (log_to_fd > 2)
		close(log_to_fd);
	if (the_channel->close)
		(*the_channel->close)();
	else
		close(devfd);	/* some plugins don't have a close function */
	close(fd_ppp);
	close(fd_devnull);
	if (infd != 0)
		close(infd);
	if (outfd != 1)
		close(outfd);
	if (errfd != 2)
		close(errfd);

	notify(fork_notifier, 0);
	close(pipefd[0]);
	/* this close unblocks the read() call above in the parent */
	close(pipefd[1]);

	return 0;
}

static bool
add_script_env(int pos, char *newstring)
{
    if (pos + 1 >= s_env_nalloc) {
	int new_n = pos + 17;
	char **newenv = realloc(script_env, new_n * sizeof(char *));
	if (newenv == NULL) {
	    free(newstring - 1);
	    return 0;
	}
	script_env = newenv;
	s_env_nalloc = new_n;
    }
    script_env[pos] = newstring;
    script_env[pos + 1] = NULL;
    return 1;
}

static void
remove_script_env(int pos)
{
    free(script_env[pos] - 1);
    while ((script_env[pos] = script_env[pos + 1]) != NULL)
	pos++;
}

/*
 * update_system_environment - process the list of set/unset options
 * and update the system environment.
 */
static void
update_system_environment(void)
{
    struct userenv *uep;

    for (uep = userenv_list; uep != NULL; uep = uep->ue_next) {
	if (uep->ue_isset)
	    setenv(uep->ue_name, uep->ue_value, 1);
	else
	    unsetenv(uep->ue_name);
    }
}

/*
 * device_script - run a program to talk to the specified fds
 * (e.g. to run the connector or disconnector script).
 * stderr gets connected to the log fd or to the PPP_PATH_CONNERRS file.
 */
int
device_script(char *program, int in, int out, int dont_wait)
{
    int pid;
    int status = -1;
    int errfd;
    int ret;

    if (log_to_fd >= 0)
	errfd = log_to_fd;
    else
	errfd = open(PPP_PATH_CONNERRS, O_WRONLY | O_APPEND | O_CREAT, 0644);

    ++conn_running;
    pid = ppp_safe_fork(in, out, errfd);

    if (pid != 0 && log_to_fd < 0)
	close(errfd);

    if (pid < 0) {
	--conn_running;
	error("Failed to create child process: %m");
	return -1;
    }

    if (pid != 0) {
	record_child(pid, program, NULL, NULL, 1);
	status = 0;
	if (!dont_wait) {
	    while (waitpid(pid, &status, 0) < 0) {
		if (errno == EINTR)
		    continue;
		fatal("error waiting for (dis)connection process: %m");
	    }
	    forget_child(pid, status);
	    --conn_running;
	}
	return (status == 0 ? 0 : -1);
    }

    /* here we are executing in the child */
    ret = setgid(getgid());
    if (ret != 0) {
        perror("pppd: setgid failed\n");
        exit(1);
    }
    ret = setuid(uid);
    if (ret != 0 || getuid() != uid) {
        perror("pppd: setuid failed\n");
        exit(1);
    }
    update_system_environment();
    execl("/bin/sh", "sh", "-c", program, (char *)0);
    perror("pppd: could not exec /bin/sh");
    _exit(99);
    /* NOTREACHED */
}


/*
 * update_script_environment - process the list of set/unset options
 * and update the script environment.  Note that we intentionally do
 * not update the TDB.  These changes are layered on top right before
 * exec.  It is not possible to use script_setenv() or
 * ppp_script_unsetenv() safely after this routine is run.
 */
static void
update_script_environment(void)
{
    struct userenv *uep;

    for (uep = userenv_list; uep != NULL; uep = uep->ue_next) {
	int i;
	char *p, *newstring;
	int nlen = strlen(uep->ue_name);

	for (i = 0; (p = script_env[i]) != NULL; i++) {
	    if (strncmp(p, uep->ue_name, nlen) == 0 && p[nlen] == '=')
		break;
	}
	if (uep->ue_isset) {
	    nlen += strlen(uep->ue_value) + 2;
	    newstring = malloc(nlen + 1);
	    if (newstring == NULL)
		continue;
	    *newstring++ = 0;
	    slprintf(newstring, nlen, "%s=%s", uep->ue_name, uep->ue_value);
	    if (p != NULL)
		script_env[i] = newstring;
	    else
		add_script_env(i, newstring);
	} else if (p != NULL) {
	    remove_script_env(i);
	}
    }
}

/*
 * run_program - execute a program with given arguments,
 * but don't wait for it unless wait is non-zero.
 * If the program can't be executed, logs an error unless
 * must_exist is 0 and the program file doesn't exist.
 * Returns -1 if it couldn't fork, 0 if the file doesn't exist
 * or isn't an executable plain file, or the process ID of the child.
 * If done != NULL, (*done)(arg) will be called later (within
 * reap_kids) iff the return value is > 0.
 */
pid_t
run_program(char *prog, char * const *args, int must_exist, void (*done)(void *), void *arg, int wait)
{
    int pid, status, ret;
    struct stat sbuf;

    /*
     * First check if the file exists and is executable.
     * We don't use access() because that would use the
     * real user-id, which might not be root, and the script
     * might be accessible only to root.
     */
    errno = EINVAL;
    if (stat(prog, &sbuf) < 0 || !S_ISREG(sbuf.st_mode)
	|| (sbuf.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH)) == 0) {
	if (must_exist || errno != ENOENT)
	    warn("Can't execute %s: %m", prog);
	return 0;
    }

    pid = ppp_safe_fork(fd_devnull, fd_devnull, fd_devnull);
    if (pid == -1) {
	error("Failed to create child process for %s: %m", prog);
	return -1;
    }
    if (pid != 0) {
	if (debug)
	    dbglog("Script %s started (pid %d)", prog, pid);
	record_child(pid, prog, done, arg, 0);
	if (wait) {
	    while (waitpid(pid, &status, 0) < 0) {
		if (errno == EINTR)
		    continue;
		fatal("error waiting for script %s: %m", prog);
	    }
	    forget_child(pid, status);
	}
	return pid;
    }

    /* Leave the current location */
    (void) setsid();	/* No controlling tty. */
    (void) umask (S_IRWXG|S_IRWXO);
    ret = chdir ("/");	/* no current directory. */
    if (ret != 0) {
        fatal("Failed to change directory to '/', %m");
    }
    ret = setuid(0);		/* set real UID = root */
    if (ret != 0) {
        fatal("Failed to set uid, %m");
    }
    ret = setgid(getegid());
    if (ret != 0) {
        fatal("failed to set gid, %m");
    }

#ifdef BSD
    /* Force the priority back to zero if pppd is running higher. */
    if (setpriority (PRIO_PROCESS, 0, 0) < 0)
	warn("can't reset priority to 0: %m");
#endif

    /* run the program */
    update_script_environment();
    execve(prog, args, script_env);
    if (must_exist || errno != ENOENT) {
	/* have to reopen the log, there's nowhere else
	   for the message to go. */
	reopen_log();
	syslog(LOG_ERR, "Can't execute %s: %m", prog);
	closelog();
    }
    _exit(99);
}


/*
 * record_child - add a child process to the list for reap_kids
 * to use.
 */
void
record_child(int pid, char *prog, void (*done)(void *), void *arg, int killable)
{
    struct subprocess *chp;

    ++n_children;

    chp = (struct subprocess *) malloc(sizeof(struct subprocess));
    if (chp == NULL) {
	warn("losing track of %s process", prog);
    } else {
	chp->pid = pid;
	chp->prog = prog;
	chp->done = done;
	chp->arg = arg;
	chp->next = children;
	chp->killable = killable;
	children = chp;
    }
}

/*
 * childwait_end - we got fed up waiting for the child processes to
 * exit, send them all a SIGTERM.
 */
static void
childwait_end(void *arg)
{
    struct subprocess *chp;

    for (chp = children; chp != NULL; chp = chp->next) {
	if (debug)
	    dbglog("sending SIGTERM to process %d", chp->pid);
	kill(chp->pid, SIGTERM);
    }
    childwait_done = 1;
}

/*
 * forget_child - clean up after a dead child
 */
static void
forget_child(int pid, int status)
{
    struct subprocess *chp, **prevp;

    for (prevp = &children; (chp = *prevp) != NULL; prevp = &chp->next) {
        if (chp->pid == pid) {
	    --n_children;
	    *prevp = chp->next;
	    break;
	}
    }
    if (WIFSIGNALED(status)) {
        warn("Child process %s (pid %d) terminated with signal %d",
	     (chp? chp->prog: "??"), pid, WTERMSIG(status));
    } else if (debug)
        dbglog("Script %s finished (pid %d), status = 0x%x",
	       (chp? chp->prog: "??"), pid,
	       WIFEXITED(status) ? WEXITSTATUS(status) : status);
    if (chp && chp->done)
        (*chp->done)(chp->arg);
    if (chp)
        free(chp);
}

/*
 * reap_kids - get status from any dead child processes,
 * and log a message for abnormal terminations.
 */
static int
reap_kids(void)
{
    int pid, status;

    if (n_children == 0)
	return 0;
    while ((pid = waitpid(-1, &status, WNOHANG)) != -1 && pid != 0) {
        forget_child(pid, status);
    }
    if (pid == -1) {
	if (errno == ECHILD)
	    return -1;
	if (errno != EINTR)
	    error("Error waiting for child process: %m");
    }
    return 0;
}


struct notifier **get_notifier_by_type(ppp_notify_t type)
{
    struct notifier **list[NF_MAX_NOTIFY] = {
        [NF_PID_CHANGE  ] = &pidchange,
        [NF_PHASE_CHANGE] = &phasechange,
        [NF_EXIT        ] = &exitnotify,
        [NF_SIGNALED    ] = &sigreceived,
        [NF_IP_UP       ] = &ip_up_notifier,
        [NF_IP_DOWN     ] = &ip_down_notifier,
#ifdef PPP_WITH_IPV6CP
        [NF_IPV6_UP     ] = &ipv6_up_notifier,
        [NF_IPV6_DOWN   ] = &ipv6_down_notifier,
#endif
        [NF_AUTH_UP     ] = &auth_up_notifier,
        [NF_LINK_DOWN   ] = &link_down_notifier,
        [NF_FORK        ] = &fork_notifier,
    };
    return list[type];
}

/*
 * add_notifier - add a new function to be called when something happens.
 */
void
ppp_add_notify(ppp_notify_t type, ppp_notify_fn *func, void *arg)
{
    struct notifier **notif = get_notifier_by_type(type);
    if (notif) {

	struct notifier *np = malloc(sizeof(struct notifier));
	if (np == 0)
	    novm("notifier struct");
	np->next = *notif;
	np->func = func;
	np->arg = arg;
	*notif = np;
    } else {
	error("Could not find notifier function for: %d", type);
    }
}

/*
 * remove_notifier - remove a function from the list of things to
 * be called when something happens.
 */
void
ppp_del_notify(ppp_notify_t type, ppp_notify_fn *func, void *arg)
{
    struct notifier **notif = get_notifier_by_type(type);
    if (notif) {
	struct notifier *np;

	for (; (np = *notif) != 0; notif = &np->next) {
	    if (np->func == func && np->arg == arg) {
		*notif = np->next;
		free(np);
		break;
	    }
	}
    } else {
	error("Could not find notifier function for: %d", type);
    }
}

/*
 * notify - call a set of functions registered with add_notifier.
 */
void
notify(struct notifier *notif, int val)
{
    struct notifier *np;

    while ((np = notif) != 0) {
	notif = np->next;
	(*np->func)(np->arg, val);
    }
}

/*
 * novm - log an error message saying we ran out of memory, and die.
 */
void
novm(const char *msg)
{
    fatal("Virtual memory exhausted allocating %s\n", msg);
}

/*
 * ppp_script_setenv - set an environment variable value to be used
 * for scripts that we run (e.g. ip-up, auth-up, etc.)
 */
void
ppp_script_setenv(char *var, char *value, int iskey)
{
    size_t varl = strlen(var);
    size_t vl = varl + strlen(value) + 2;
    int i;
    char *p, *newstring;

    newstring = (char *) malloc(vl+1);
    if (newstring == 0)
	return;
    *newstring++ = iskey;
    slprintf(newstring, vl, "%s=%s", var, value);

    /* check if this variable is already set */
    if (script_env != 0) {
	for (i = 0; (p = script_env[i]) != 0; ++i) {
	    if (strncmp(p, var, varl) == 0 && p[varl] == '=') {
#ifdef PPP_WITH_TDB
		if (p[-1] && pppdb != NULL)
		    delete_db_key(p);
#endif
		free(p-1);
		script_env[i] = newstring;
#ifdef PPP_WITH_TDB
		if (pppdb != NULL) {
		    if (iskey)
			add_db_key(newstring);
		    update_db_entry();
		}
#endif
		return;
	    }
	}
    } else {
	/* no space allocated for script env. ptrs. yet */
	i = 0;
	script_env = malloc(16 * sizeof(char *));
	if (script_env == 0) {
	    free(newstring - 1);
	    return;
	}
	s_env_nalloc = 16;
    }

    if (!add_script_env(i, newstring))
	return;

#ifdef PPP_WITH_TDB
    if (pppdb != NULL) {
	if (iskey)
	    add_db_key(newstring);
	update_db_entry();
    }
#endif
}

/*
 * ppp_script_unsetenv - remove a variable from the environment
 * for scripts.
 */
void
ppp_script_unsetenv(char *var)
{
    int vl = strlen(var);
    int i;
    char *p;

    if (script_env == 0)
	return;
    for (i = 0; (p = script_env[i]) != 0; ++i) {
	if (strncmp(p, var, vl) == 0 && p[vl] == '=') {
#ifdef PPP_WITH_TDB
	    if (p[-1] && pppdb != NULL)
		delete_db_key(p);
#endif
	    remove_script_env(i);
	    break;
	}
    }
#ifdef PPP_WITH_TDB
    if (pppdb != NULL)
	update_db_entry();
#endif
}

/*
 * Any arbitrary string used as a key for locking the database.
 * It doesn't matter what it is as long as all pppds use the same string.
 */
#define PPPD_LOCK_KEY	"pppd lock"

/*
 * lock_db - get an exclusive lock on the TDB database.
 * Used to ensure atomicity of various lookup/modify operations.
 */
void lock_db(void)
{
#ifdef PPP_WITH_TDB
	TDB_DATA key;

	key.dptr = PPPD_LOCK_KEY;
	key.dsize = strlen(key.dptr);
	tdb_chainlock(pppdb, key);
#endif
}

/*
 * unlock_db - remove the exclusive lock obtained by lock_db.
 */
void unlock_db(void)
{
#ifdef PPP_WITH_TDB
	TDB_DATA key;

	key.dptr = PPPD_LOCK_KEY;
	key.dsize = strlen(key.dptr);
	tdb_chainunlock(pppdb, key);
#endif
}

#ifdef PPP_WITH_TDB
/*
 * update_db_entry - update our entry in the database.
 */
static void
update_db_entry(void)
{
    TDB_DATA key, dbuf;
    int vlen, i;
    char *p, *q, *vbuf;

    if (script_env == NULL)
	return;
    vlen = 0;
    for (i = 0; (p = script_env[i]) != 0; ++i)
	vlen += strlen(p) + 1;
    vbuf = malloc(vlen + 1);
    if (vbuf == 0)
	novm("database entry");
    q = vbuf;
    for (i = 0; (p = script_env[i]) != 0; ++i)
	q += slprintf(q, vbuf + vlen - q, "%s;", p);

    key.dptr = db_key;
    key.dsize = strlen(db_key);
    dbuf.dptr = vbuf;
    dbuf.dsize = vlen;
    if (tdb_store(pppdb, key, dbuf, TDB_REPLACE))
	error("tdb_store failed: %s", tdb_errorstr(pppdb));

    if (vbuf)
        free(vbuf);

}

/*
 * add_db_key - add a key that we can use to look up our database entry.
 */
static void
add_db_key(const char *str)
{
    TDB_DATA key, dbuf;

    key.dptr = (char *) str;
    key.dsize = strlen(str);
    dbuf.dptr = db_key;
    dbuf.dsize = strlen(db_key);
    if (tdb_store(pppdb, key, dbuf, TDB_REPLACE))
	error("tdb_store key failed: %s", tdb_errorstr(pppdb));
}

/*
 * delete_db_key - delete a key for looking up our database entry.
 */
static void
delete_db_key(const char *str)
{
    TDB_DATA key;

    key.dptr = (char *) str;
    key.dsize = strlen(str);
    tdb_delete(pppdb, key);
}

/*
 * cleanup_db - delete all the entries we put in the database.
 */
static void
cleanup_db(void)
{
    TDB_DATA key;
    int i;
    char *p;

    key.dptr = db_key;
    key.dsize = strlen(db_key);
    tdb_delete(pppdb, key);
    for (i = 0; (p = script_env[i]) != 0; ++i)
	if (p[-1])
	    delete_db_key(p);
}
#endif /* PPP_WITH_TDB */

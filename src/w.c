/*
 * w - show what logged in users are doing.
 *
 * Almost entirely rewritten from scratch by Charles Blake circa
 * June 1996. Some vestigal traces of the original may exist.
 * That was done in 1993 by Larry Greenfield with some fixes by
 * Michael K. Johnson.
 *
 * Changes by Albert Cahalan, 2002.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <locale.h>
#include <pwd.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_UTMPX_H
#	include <utmpx.h>
#else
#	include <utmp.h>
#endif
#include <arpa/inet.h>

#include "c.h"
#include "fileutils.h"
#include "nls.h"

#include "misc.h"
#include "pids.h"

static int ignoreuser = 0;	/* for '-u' */
static int oldstyle = 0;	/* for '-o' */

#ifdef HAVE_UTMPX_H
typedef struct utmpx utmp_t;
#else
typedef struct utmp utmp_t;
#endif

#if !defined(UT_HOSTSIZE) || defined(__UT_HOSTSIZE)
#	define UT_HOSTSIZE __UT_HOSTSIZE
#	define UT_LINESIZE __UT_LINESIZE
#	define UT_NAMESIZE __UT_NAMESIZE
#endif

#ifdef W_SHOWFROM
# define FROM_STRING "on"
#else
# define FROM_STRING "off"
#endif

#define MAX_CMD_WIDTH	512
#define MIN_CMD_WIDTH   7

/*
 * This routine is careful since some programs leave utmp strings
 * unprintable. Always outputs at least 16 chars padded with
 * spaces on the right if necessary.
 */
static void print_host(const char *restrict host, int len, const int fromlen)
{
	const char *last;
	int width = 0;

	if (len > fromlen)
		len = fromlen;
	last = host + len;
	for (; host < last; host++) {
		if (*host == '\0') break;
		if (isprint(*host) && *host != ' ') {
			fputc(*host, stdout);
			++width;
		} else {
			fputc('-', stdout);
			++width;
			break;
		}
	}

	/*
	 * space-fill, and a '-' too if needed to ensure the
	 * column exists
	 */
	if (!width) {
		fputc('-', stdout);
		++width;
	}
	while (width++ < fromlen)
		fputc(' ', stdout);
}


/* This routine prints the display part of the host or IPv6 link address interface */
static void print_display_or_interface(const char *restrict host, int len, int restlen)
{
	const char *const end = host + (len > 0 ? len : 0);
	const char *disp, *tmp;

	if (restlen <= 0) return; /* not enough space for printing anything */

	/* search for a collon (might be a display) */
	disp = host;
	while ( (disp < end) && (*disp != ':') && isprint(*disp) ) disp++;

	/* colon found */
	if (disp < end && *disp == ':') {
		/* detect multiple colons -> IPv6 in the host (not a display) */
		tmp = disp+1;
		while ( (tmp < end) && (*tmp != ':') && isprint(*tmp) ) tmp++;

		if (tmp >= end || *tmp != ':') { /* multiple colons not found - it's a display */

			/* number of chars till the end of the input field */
			len -= (disp - host);

			/* if it is still longer than the rest of the output field, then cut it */
			if (len > restlen) len = restlen;

			/* print the display */
			while ((len > 0) && isprint(*disp) && (*disp != ' ')) {
				len--; restlen--;
				fputc(*disp, stdout);
				disp++;
			}

			if ((len > 0) && (*disp != '\0')) { /* space or nonprintable found - replace with dash and stop printing */
				restlen--;
				fputc('-', stdout);
			}
		} else { /* multiple colons found - it's an IPv6 address */

			/* search for % (interface separator in case of IPv6 link address) */
			while ( (tmp < end) && (*tmp != '%') && isprint(*tmp) ) tmp++;

			if (tmp < end && *tmp == '%') { /* interface separator found */

				/* number of chars till the end of the input field */
				len -= (tmp - host);

				/* if it is still longer than the rest of the output field, then cut it */
				if (len > restlen) len = restlen;

				/* print the interface */
				while ((len > 0) && isprint(*tmp) && (*tmp != ' ')) {
					len--; restlen--;
					fputc(*tmp, stdout);
					tmp++;
				}
				if ((len > 0) && (*tmp != '\0')) {  /* space or nonprintable found - replace with dash and stop printing */
					restlen--;
					fputc('-', stdout);
				}
			}
		}
	}

	/* padding with spaces */
	while (restlen > 0) {
		fputc(' ', stdout);
		restlen--;
	}
}


/* This routine prints either the hostname or the IP address of the remote */
static void print_from(const utmp_t *restrict const u, const int ip_addresses, const int fromlen) {
	char buf[fromlen + 1];
	char buf_ipv6[INET6_ADDRSTRLEN];
	int len;
#ifndef __CYGWIN__
	int32_t ut_addr_v6[4];      /* IP address of the remote host */

	if (ip_addresses) { /* -i switch used */
		memcpy(&ut_addr_v6, &u->ut_addr_v6, sizeof(ut_addr_v6));
		if (IN6_IS_ADDR_V4MAPPED(&ut_addr_v6)) {
			/* map back */
			ut_addr_v6[0] = ut_addr_v6[3];
			ut_addr_v6[1] = 0;
			ut_addr_v6[2] = 0;
			ut_addr_v6[3] = 0;
		}
		if (ut_addr_v6[1] || ut_addr_v6[2] || ut_addr_v6[3]) {
			/* IPv6 */
			if (!inet_ntop(AF_INET6, &ut_addr_v6, buf_ipv6, sizeof(buf_ipv6))) {
				strcpy(buf, ""); /* invalid address, clean the buffer */
			} else {
				strncpy(buf, buf_ipv6, fromlen); /* address valid, copy to buffer */
			}
		} else {
			/* IPv4 */
			if (!(ut_addr_v6[0] && inet_ntop(AF_INET, &ut_addr_v6[0], buf, sizeof(buf)))) {
				strcpy(buf, ""); /* invalid address, clean the buffer */
			}
		}
		buf[fromlen] = '\0';

		len = strlen(buf);
		if (len) { /* IP address is non-empty, print it (and concatenate with display, if present) */
			fputs(buf, stdout);
			/* show the display part of the host or IPv6 link addr. interface, if present */
			print_display_or_interface(u->ut_host, UT_HOSTSIZE, fromlen - len);
		} else { /* IP address is empty, print the host instead */
			print_host(u->ut_host, UT_HOSTSIZE, fromlen);
		}
	} else {  /* -i switch NOT used */
		print_host(u->ut_host, UT_HOSTSIZE, fromlen);
	}
#else
	print_host(u->ut_host, UT_HOSTSIZE, fromlen);
#endif
}


/* compact 7 char format for time intervals (belongs in libproc?) */
static void print_time_ival7(time_t t, int centi_sec, FILE * fout)
{
	if ((long)t < (long)0) {
		/* system clock changed? */
		printf("   ?   ");
		return;
	}
	if (oldstyle) {
		if (t >= 48 * 60 * 60)
			/* > 2 days */
			fprintf(fout, _(" %2lludays"), (unsigned long long)t / (24 * 60 * 60));
		else if (t >= 60 * 60)
			/* > 1 hour */
		        /* Translation Hint: Hours:Minutes */
			fprintf(fout, " %2llu:%02u ", (unsigned long long)t / (60 * 60),
				(unsigned)((t / 60) % 60));
		else if (t > 60)
			/* > 1 minute */
		        /* Translation Hint: Minutes:Seconds */
			fprintf(fout, _(" %2llu:%02um"), (unsigned long long)t / 60, (unsigned)t % 60);
		else
			fprintf(fout, "       ");
	} else {
		if (t >= 48 * 60 * 60)
			/* 2 days or more */
			fprintf(fout, _(" %2lludays"), (unsigned long long)t / (24 * 60 * 60));
		else if (t >= 60 * 60)
			/* 1 hour or more */
		        /* Translation Hint: Hours:Minutes */
			fprintf(fout, _(" %2llu:%02um"), (unsigned long long)t / (60 * 60),
				(unsigned)((t / 60) % 60));
		else if (t > 60)
			/* 1 minute or more */
		        /* Translation Hint: Minutes:Seconds */
			fprintf(fout, " %2llu:%02u ", (unsigned long long)t / 60, (unsigned)t % 60);
		else
		        /* Translation Hint: Seconds:Centiseconds */
			fprintf(fout, _(" %2llu.%02us"), (unsigned long long)t, centi_sec);
	}
}

/* stat the device file to get an idle time */
static time_t idletime(const char *restrict const tty)
{
	struct stat sbuf;
	if (stat(tty, &sbuf) != 0)
		return 0;
	return time(NULL) - sbuf.st_atime;
}

/* 7 character formatted login time */

static void print_logintime(time_t logt, FILE * fout)
{

	/* Abbreviated of weekday can be longer than 3 characters,
	 * see for instance hu_HU.  Using 16 is few bytes more than
	 * enough.  */
	char time_str[16];
	time_t curt;
	struct tm *logtm, *curtm;
	int today;

	curt = time(NULL);
	curtm = localtime(&curt);
	/* localtime returns a pointer to static memory */
	today = curtm->tm_yday;
	logtm = localtime(&logt);
	if (curt - logt > 12 * 60 * 60 && logtm->tm_yday != today) {
		if (curt - logt > 6 * 24 * 60 * 60) {
		        strftime(time_str, sizeof(time_str), "%b", logtm);
			fprintf(fout, " %02d%3s%02d", logtm->tm_mday,
				time_str, logtm->tm_year % 100);
		} else {
		        strftime(time_str, sizeof(time_str), "%a", logtm);
			fprintf(fout, " %3s%02d  ", time_str,
				logtm->tm_hour);
		}
	} else {
		fprintf(fout, " %02d:%02d  ", logtm->tm_hour, logtm->tm_min);
	}
}

/*
 * Get the Device ID of the given TTY
 */
static int get_tty_device(const char *restrict const name)
{
    struct stat st;
    static char buf[32];
    char *dev_paths[] = { "/dev/%s", "/dev/tty%s", "/dev/pts/%s", NULL};
    int i;

    if (name[0] == '/' && stat(name, &st) == 0)
        return st.st_rdev;

    for (i=0; dev_paths[i] != NULL; i++) {
        snprintf(buf, 32, dev_paths[i], name);
        if (stat(buf, &st) == 0)
            return st.st_rdev;
    }
    return -1;
}

/*
 * This function scans the process table accumulating total cpu
 * times for any processes "associated" with this login session.
 * It also searches for the "best" process to report as "(w)hat"
 * the user for that login session is doing currently. This the
 * essential core of 'w'.
 */
static int find_best_proc(
        const utmp_t * restrict const u,
        const char *restrict const tty,
        unsigned long long *restrict const jcpu,
        unsigned long long *restrict const pcpu,
        char *cmdline)
{
#define PIDS_GETINT(e) PIDS_VAL(EU_ ## e, s_int, reap->stacks[i], info)
#define PIDS_GETUNT(e) PIDS_VAL(EU_ ## e, u_int, reap->stacks[i], info)
#define PIDS_GETULL(e) PIDS_VAL(EU_ ## e, ull_int, reap->stacks[i], info)
#define PIDS_GETSTR(e) PIDS_VAL(EU_ ## e, str, reap->stacks[i], info)
    unsigned uid = ~0U;
    int found_utpid = 0;
    int i, total_procs, line;
    unsigned long long best_time = 0;
    unsigned long long secondbest_time = 0;

    struct pids_info *info=NULL;
    struct pids_fetch *reap;
    enum pids_item items[] = {
        PIDS_ID_TGID,
        PIDS_TICS_BEGAN,
        PIDS_ID_EUID,
        PIDS_ID_RUID,
        PIDS_ID_TPGID,
        PIDS_ID_PGRP,
        PIDS_TTY,
        PIDS_TICS_ALL,
        PIDS_CMDLINE};
    enum rel_items {
        EU_TGID, EU_START, EU_EUID, EU_RUID, EU_TPGID, EU_PGRP, EU_TTY,
        EU_TICS_ALL, EU_CMDLINE};

    *jcpu = 0;
    *pcpu = 0;
    if (!ignoreuser) {
        char buf[UT_NAMESIZE + 1];
        struct passwd *passwd_data;
        strncpy(buf, u->ut_user, UT_NAMESIZE);
        buf[UT_NAMESIZE] = '\0';
        if ((passwd_data = getpwnam(buf)) == NULL)
            return 0;
        uid = passwd_data->pw_uid;
        /* OK to have passwd_data go out of scope here */
    }

    line = get_tty_device(tty);

    if (procps_pids_new(&info, items, 9) < 0)
        xerrx(EXIT_FAILURE,
              _("Unable to create pid info structure"));
    if ((reap = procps_pids_reap(info, PIDS_FETCH_TASKS_ONLY)) == NULL)
        xerrx(EXIT_FAILURE,
              _("Unable to load process information"));
    total_procs = reap->counts->total;

    for (i=0; i < total_procs; i++) {
        /* is this the login process? */
        if (PIDS_GETINT(TGID) == u->ut_pid) {
            found_utpid = 1;
            if (!best_time) {
                best_time = PIDS_GETULL(START);
                strncpy(cmdline, PIDS_GETSTR(CMDLINE), MAX_CMD_WIDTH);
                *pcpu = PIDS_GETULL(TICS_ALL);
            }

        }
        if (PIDS_GETINT(TTY) != line)
            continue;
        (*jcpu) += PIDS_VAL(EU_TICS_ALL, ull_int, reap->stacks[i], info);
        if (!(secondbest_time && PIDS_GETULL(START) <= secondbest_time)) {
            secondbest_time = PIDS_GETULL(START);
            if (cmdline[0] == '-' && cmdline[1] == '\0') {
                strncpy(cmdline, PIDS_GETSTR(CMDLINE), MAX_CMD_WIDTH);
                *pcpu = PIDS_GETULL(TICS_ALL);
            }
        }
        if (
            (!ignoreuser && uid != PIDS_GETUNT(EUID)
             && uid != PIDS_GETUNT(RUID))
            || (PIDS_GETINT(PGRP) != PIDS_GETINT(TPGID))
            || (PIDS_GETULL(START) <= best_time)
           )
            continue;
        best_time = PIDS_GETULL(START);
        strncpy(cmdline, PIDS_GETSTR(CMDLINE), MAX_CMD_WIDTH);
        *pcpu = PIDS_GETULL(TICS_ALL);
    }
    procps_pids_unref(&info);
    return found_utpid;
#undef PIDS_GETINT
#undef PIDS_GETUNT
#undef PIDS_GETULL
#undef PIDS_GETSTR
}

static void showinfo(
            utmp_t * u, int formtype, int maxcmd, int from,
            const int userlen, const int fromlen, const int ip_addresses)
{
    unsigned long long jcpu, pcpu;
    unsigned i;
    char uname[UT_NAMESIZE + 1] = "", tty[5 + UT_LINESIZE + 1] = "/dev/";
    long hertz;
    char cmdline[MAX_CMD_WIDTH + 1];

    strcpy(cmdline, "-");

    hertz = procps_hertz_get();
    for (i = 0; i < UT_LINESIZE; i++)
        /* clean up tty if garbled */
        if (isalnum(u->ut_line[i]) || (u->ut_line[i] == '/'))
            tty[i + 5] = u->ut_line[i];
        else
            tty[i + 5] = '\0';

    if (find_best_proc(u, tty + 5, &jcpu, &pcpu, cmdline) == 0)
    /*
     * just skip if stale utmp entry (i.e. login proc doesn't
     * exist). If there is a desire a cmdline flag could be
     * added to optionally show it with a prefix of (stale)
     * in front of cmd or something like that.
     */
        return;

    /* force NUL term for printf */
    strncpy(uname, u->ut_user, UT_NAMESIZE);

    if (formtype) {
        printf("%-*.*s%-9.8s", userlen + 1, userlen, uname, u->ut_line);
        if (from)
            print_from(u, ip_addresses, fromlen);
#ifdef HAVE_UTMPX_H
        print_logintime(u->ut_tv.tv_sec, stdout);
#else
        print_logintime(u->ut_time, stdout);
#endif
        if (*u->ut_line == ':')
            /* idle unknown for xdm logins */
            printf(" ?xdm? ");
        else
            print_time_ival7(idletime(tty), 0, stdout);
        print_time_ival7(jcpu / hertz, (jcpu % hertz) * (100. / hertz),
                 stdout);
        if (pcpu > 0)
            print_time_ival7(pcpu / hertz,
                             (pcpu % hertz) * (100. / hertz),
                             stdout);
        else
            printf("   ?   ");
    } else {
        printf("%-*.*s%-9.8s", userlen + 1, userlen, u->ut_user,
               u->ut_line);
        if (from)
            print_from(u, ip_addresses, fromlen);
        if (*u->ut_line == ':')
            /* idle unknown for xdm logins */
            printf(" ?xdm? ");
        else
            print_time_ival7(idletime(tty), 0, stdout);
    }
    printf(" %.*s\n", maxcmd, cmdline);
}

static void __attribute__ ((__noreturn__))
    usage(FILE * out)
{
	fputs(USAGE_HEADER, out);
	fprintf(out,
              _(" %s [options] [user]\n"), program_invocation_short_name);
	fputs(USAGE_OPTIONS, out);
	fputs(_(" -h, --no-header     do not print header\n"),out);
	fputs(_(" -u, --no-current    ignore current process username\n"),out);
	fputs(_(" -s, --short         short format\n"),out);
	fputs(_(" -f, --from          show remote hostname field\n"),out);
	fputs(_(" -o, --old-style     old style output\n"),out);
	fputs(_(" -i, --ip-addr       display IP address instead of hostname (if possible)\n"), out);
	fputs(USAGE_SEPARATOR, out);
	fputs(_("     --help     display this help and exit\n"), out);
	fputs(USAGE_VERSION, out);
	fprintf(out, USAGE_MAN_TAIL("w(1)"));

	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	char *user = NULL, *p;
	utmp_t *u;
	struct winsize win;
	int ch;
	int maxcmd = 80;
	int userlen = 8;
	int fromlen = 16;
	char *env_var;

	/* switches (defaults) */
	int header = 1;
	int longform = 1;
	int from = 1;
	int ip_addresses = 0;

	enum {
		HELP_OPTION = CHAR_MAX + 1
	};

	static const struct option longopts[] = {
		{"no-header", no_argument, NULL, 'h'},
		{"no-current", no_argument, NULL, 'u'},
		{"short", no_argument, NULL, 's'},
		{"from", no_argument, NULL, 'f'},
		{"old-style", no_argument, NULL, 'o'},
		{"ip-addr", no_argument, NULL, 'i'},
		{"help", no_argument, NULL, HELP_OPTION},
		{"version", no_argument, NULL, 'V'},
		{NULL, 0, NULL, 0}
	};

#ifdef HAVE_PROGRAM_INVOCATION_NAME
	program_invocation_name = program_invocation_short_name;
#endif
	setlocale (LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

#ifndef W_SHOWFROM
	from = 0;
#endif

	while ((ch =
		getopt_long(argc, argv, "husfoVi", longopts, NULL)) != -1)
		switch (ch) {
		case 'h':
			header = 0;
			break;
		case 's':
			longform = 0;
			break;
		case 'f':
			from = !from;
			break;
		case 'V':
			printf(PROCPS_NG_VERSION);
			exit(0);
		case 'u':
			ignoreuser = 1;
			break;
		case 'o':
			oldstyle = 1;
			break;
		case 'i':
			ip_addresses = 1;
			from = 1;
			break;
		case HELP_OPTION:
			usage(stdout);
		default:
			usage(stderr);
		}

	if ((argv[optind]))
		user = (argv[optind]);

	/* Get user field length from environment */
	if ((env_var = getenv("PROCPS_USERLEN")) != NULL) {
		int ut_namesize = UT_NAMESIZE;
		userlen = atoi(env_var);
		if (userlen < 8 || ut_namesize < userlen) {
			xwarnx
			    (_("User length environment PROCPS_USERLEN must be between 8 and %i, ignoring.\n"),
			     ut_namesize);
			userlen = 8;
		}
	}
	/* Get from field length from environment */
	if ((env_var = getenv("PROCPS_FROMLEN")) != NULL) {
		fromlen = atoi(env_var);
		if (fromlen < 8 || UT_HOSTSIZE < fromlen) {
			xwarnx
			    (_("from length environment PROCPS_FROMLEN must be between 8 and %d, ignoring\n"),
			     UT_HOSTSIZE);
			fromlen = 16;
		}
	}
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &win) != -1 && win.ws_col > 0)
		maxcmd = win.ws_col;
	else if ((p = getenv("COLUMNS")))
		maxcmd = atoi(p);
	else
		maxcmd = MAX_CMD_WIDTH;
#define CLAMP_CMD_WIDTH(cw) do { \
	if ((cw) < MIN_CMD_WIDTH) (cw) = MIN_CMD_WIDTH; \
	if ((cw) > MAX_CMD_WIDTH) (cw) = MAX_CMD_WIDTH; \
} while (0)
	CLAMP_CMD_WIDTH(maxcmd);
	maxcmd -= 21 + userlen + (from ? fromlen : 0) + (longform ? 20 : 0);
	CLAMP_CMD_WIDTH(maxcmd);
#undef CLAMP_CMD_WIDTH


	if (header) {
		/* print uptime and headers */
		printf("%s\n", procps_uptime_sprint());
		/* Translation Hint: Following five uppercase messages are
		 * headers. Try to keep alignment intact.  */
		printf(_("%-*s TTY      "), userlen, _("USER"));
		if (from)
			printf("%-*s", fromlen - 1, _("FROM"));
		if (longform)
			printf(_("  LOGIN@   IDLE   JCPU   PCPU WHAT\n"));
		else
			printf(_("   IDLE WHAT\n"));
	}

#ifdef HAVE_UTMPX_H
	setutxent();
#else
	utmpname(UTMP_FILE);
	setutent();
#endif
	if (user) {
		for (;;) {
#ifdef HAVE_UTMPX_H
			u = getutxent();
#else
			u = getutent();
#endif
			if (!u)
				break;
			if (u->ut_type != USER_PROCESS)
				continue;
			if (!strncmp(u->ut_user, user, UT_NAMESIZE))
				showinfo(u, longform, maxcmd, from, userlen,
					 fromlen, ip_addresses);
		}
	} else {
		for (;;) {
#ifdef HAVE_UTMPX_H
			u = getutxent();
#else
			u = getutent();
#endif
			if (!u)
				break;
			if (u->ut_type != USER_PROCESS)
				continue;
			if (*u->ut_user)
				showinfo(u, longform, maxcmd, from, userlen,
					 fromlen, ip_addresses);
		}
	}
#ifdef HAVE_UTMPX_H
	endutxent();
#else
	endutent();
#endif

	return EXIT_SUCCESS;
}

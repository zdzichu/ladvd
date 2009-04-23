/*
 $Id$
*/

#include "common.h"
#include "util.h"
#include "protos.h"
#include "main.h"
#include <ctype.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>

extern int8_t loglevel;
uint32_t options = OPT_DAEMON;

void usage();
void queue_msg(int fd, short event, int *cfd);

int main(int argc, char *argv[]) {

    int ch, i, p, run_once = 0;
    char *username = PACKAGE_USER;
#ifndef __APPLE__
    char *pidfile = PACKAGE_PID_FILE;
    char pidstr[16];
    int fd = -1;
#endif /* __APPLE__ */
    struct passwd *pwd = NULL;

    // save argc/argv
    int sargc;
    char **sargv;

    // sysinfo
    struct sysinfo sysinfo;

    // sockets
    int cpair[2], mpair[2], cfd, mfd;

    // pids
    pid_t pid;

    // packets
    struct master_msg mreq, *msg = NULL, *nmsg = NULL;
    TAILQ_INIT(&mqueue);

    // interfaces
    struct netif *netif = NULL, *subif = NULL;
    TAILQ_INIT(&netifs);
    uint16_t netifc = 0;

    // receiving
    struct event evmsg;
    struct timeval tv;

    // clear sysinfo
    memset(&sysinfo, 0, sizeof(struct sysinfo));

    // Save argv. Duplicate so setproctitle emulation doesn't clobber it
    sargc = argc;
    sargv = my_calloc(argc + 1, sizeof(*sargv));
    for (i = 0; i < argc; i++)
	sargv[i] = my_strdup(argv[i]);
    sargv[i] = NULL;

#ifndef HAVE_SETPROCTITLE
    /* Prepare for later setproctitle emulation */
    compat_init_setproctitle(argc, argv);
    argv = sargv;
#endif

    while ((ch = getopt(argc, argv, "adfhm:noru:vzc:l:CEFN")) != -1) {
	switch(ch) {
	    case 'a':
		options |= OPT_AUTO | OPT_RECV;
		break;
	    case 'd':
		loglevel = DEBUG;
		options &= ~OPT_DAEMON;
		break;
	    case 'f':
		options &= ~OPT_DAEMON;
		break;
	    case 'm':
		if ( (inet_pton(AF_INET, optarg, &sysinfo.maddr4) != 1) &&
		     (inet_pton(AF_INET6, optarg, &sysinfo.maddr6) != 1) ) {
		    my_log(CRIT, "invalid management address %s", optarg);
		    usage();
		}
		break;
	    case 'n':
		options |= OPT_MADDR;
		break;
	    case 'o':
		options |= OPT_ONCE;
		break;
	    case 'r':
		options |= OPT_RECV;
		break;
	    case 'u':
		username = optarg;
		break;
	    case 'v':
		loglevel++;
		break;
	    case 'z':
		options |= OPT_RECV | OPT_DESCR;
		break;
	    case 'c':
		// two-letter ISO 3166 country code
		if (strlen(optarg) != 2)
		    usage();
		// in capital ASCII letters
		sysinfo.country[0] = toupper(optarg[0]);
		sysinfo.country[1] = toupper(optarg[1]);
		break;
	    case 'l':
		if (strlcpy(sysinfo.location, optarg, 
			sizeof(sysinfo.location)) == 0)
		    usage();
		break;
	    case 'C':
		protos[PROTO_CDP].enabled = 1;
		break;
	    case 'E':
		protos[PROTO_EDP].enabled = 1;
		break;
	    case 'F':
		protos[PROTO_FDP].enabled = 1;
		break;
	    case 'N':
		protos[PROTO_NDP].enabled = 1;
		break;
	    default:
		usage();
	}
    }

    sargc -= optind;
    sargv += optind;

    // validate interfaces
    netifc = netif_fetch(sargc, sargv, &sysinfo, &netifs);
    if (netifc == 0)
	my_fatal("unable fetch interfaces");

    // validate username
    if ((loglevel < DEBUG) && (pwd = getpwnam(username)) == NULL)
	my_fatal("user %s does not exist", username);

    // fetch system details
    sysinfo_fetch(&sysinfo);

#ifndef __APPLE__
    // open pidfile
    if (options & OPT_DAEMON) {
	fd = open(pidfile, O_WRONLY|O_CREAT, 0666);
	if (fd == -1)
	    my_fatal("failed to open pidfile %s: %s", pidfile, strerror(errno));
	if (flock(fd, LOCK_EX|LOCK_NB) == -1)
	    my_fatal(PACKAGE_NAME " already running (%s locked)", pidfile);
    }
#endif /* __APPLE__ */

#ifndef __APPLE__
    // daemonize
    if (options & OPT_DAEMON) {
	if (daemon(0,0) == -1)
	    my_fatal("backgrounding failed: %s", strerror(errno));

	if ((snprintf(pidstr, sizeof(pidstr), "%d\n", (int)getpid()) <= 0) ||
	    (write(fd, pidstr, strlen(pidstr)) <= 0))
	    my_fatal("failed to write pidfile: %s", strerror(errno));
    }
#endif /* __APPLE__ */

    // create cmd/msg socketpair
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, cpair) == -1)
	my_fatal("cmd socketpair creation failed: %s", strerror(errno));

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, mpair) == -1)
	my_fatal("msg socketpair creation failed: %s", strerror(errno));

    // create privsep parent / child
    pid = fork();

    // quit on failure
    if (pid == -1)
	my_fatal("privsep fork failed: %s", strerror(errno));

    // this is the parent
    if (pid != 0) {

	// cleanup
	close(cpair[0]);
	close(mpair[0]);

	// enter the master loop
	master_init(&netifs, netifc, sargc, cpair[1], mpair[1]);

	// not reached
	my_fatal("master process failed");

    } else {
	// cleanup
	close(cpair[1]);
	close(mpair[1]);

	cfd = cpair[0];
	mfd = mpair[0];

	if (loglevel < DEBUG)
	    my_drop_privs(pwd);
	setproctitle("child");
    }


    // startup message
    my_log(CRIT, PACKAGE_STRING " running");

    while (cfd) {

	// create netifs
	my_log(INFO, "fetching all interfaces"); 
	if (netif_fetch(sargc, sargv, &sysinfo, &netifs) == 0) {
	    my_log(CRIT, "unable fetch interfaces");
	    goto sleep;
	}

	netif = NULL;
	while ((netif = netif_iter(netif, &netifs, sargc)) != NULL) {

	    my_log(INFO, "starting loop with interface %s", netif->name); 

	    subif = NULL;
	    while ((subif = subif_iter(subif, netif)) != NULL) {

		// populate mreq
		memset(&mreq, 0, sizeof(mreq));
		mreq.index = subif->index;
		strlcpy(mreq.name, subif->name, IFNAMSIZ);
		mreq.cmd = MASTER_SEND;

		// fetch interface media status
		my_log(INFO, "fetching %s media details", subif->name);
		if (netif_media(cfd, subif) == EXIT_FAILURE) {
		    my_log(CRIT, "error fetching interface media details");
		}

		// generate and send packets
		for (p = 0; protos[p].name != NULL; p++) {

		    // only enabled protos
		    if (!(protos[p].enabled) && !(netif->protos & (1 << p)))
			continue;

		    // clear packet
		    memset(mreq.msg, 0, ETHER_MAX_LEN);

		    my_log(INFO, "building %s packet for %s", 
				  protos[p].name, subif->name);
		    mreq.len = protos[p].build_msg(mreq.msg, subif, &sysinfo);

		    if (mreq.len == 0) {
			my_log(CRIT, "can't generate %s packet for %s",
				  protos[p].name, subif->name);
			continue;
		    }

		    // write it to the wire.
		    my_log(INFO, "sending %s packet (%d bytes) on %s",
				  protos[p].name, mreq.len, subif->name);
		    if (my_msend(cfd, &mreq) != mreq.len) {
			my_log(CRIT, "network transmit error on %s",
				  subif->name);
		    }
		}
	    }
	}

sleep:
	if (run_once == 1)
	    return (EXIT_SUCCESS);

	if ((options & OPT_RECV) == 0) {
	    my_log(INFO, "sleeping for %d seconds", SLEEPTIME);
	    sleep(SLEEPTIME);
	    continue;
	}

	// fetch time
	if (gettimeofday(&tv, NULL) != 0)
	    continue;
	tv.tv_sec += SLEEPTIME;

	event_set(&evmsg, mfd, EV_READ|EV_PERSIST, (void *)queue_msg, &cfd);
	event_add(&evmsg, &tv);
	event_loop(EVLOOP_ONCE);

	// remove expired messages
	TAILQ_FOREACH_SAFE(msg, &mqueue, entries, nmsg) {
	    if (msg->ttl < tv.tv_sec) {
		TAILQ_REMOVE(&mqueue, msg, entries);
		free(msg);
	    }
	}
    }

    return (EXIT_FAILURE);
}


void queue_msg(int fd, short event, int *cfd) {

    struct master_msg rmsg, *msg = NULL, *nmsg = NULL;
    struct netif *netif = NULL;
    unsigned int len;

    len = recv(fd, &rmsg, MASTER_MSG_SIZE, MSG_DONTWAIT);
    if (len < MASTER_MSG_SIZE)
	return;
    if (rmsg.cmd != MASTER_RECV)
	return;

    TAILQ_FOREACH(msg, &mqueue, entries) {
	// match ifindex
	if (rmsg.index != msg->index)
	    continue;
	// match protocol
	if (rmsg.proto != msg->proto)
	    continue;
	// identical source & destination
	if (memcmp(rmsg.msg, msg->msg, ETHER_ADDR_LEN * 2) != 0)
	    continue;

       nmsg = msg;
       break;
    }

    if (nmsg != NULL)
       TAILQ_REMOVE(&mqueue, msg, entries);
    else
       nmsg = my_malloc(MASTER_MSG_SIZE);

    memcpy(nmsg, &rmsg, MASTER_MSG_SIZE);
    TAILQ_INSERT_TAIL(&mqueue, nmsg, entries);

    if (options & (OPT_AUTO|OPT_DESCR))
	netif = netif_byindex(&netifs, msg->index);

    // enable the received protocol
    if ((options & OPT_AUTO) && (netif != NULL))
	    netif->protos |= (1 << msg->proto);

    // save the received name to ifdescr
    if ((options & OPT_DESCR) && (netif != NULL)) {
	memset(&rmsg, 0, sizeof(rmsg));
	rmsg.index = netif->index;
	strlcpy(rmsg.name, netif->name, IFNAMSIZ);
	rmsg.cmd = MASTER_DESCR;
	rmsg.len = protos[nmsg->proto].fetch_name(nmsg->msg, rmsg.msg);

	if (my_msend(*cfd, &rmsg) != rmsg.len) {
	    my_log(CRIT, "ifdescr ioctl failed on %s", netif->name);
	}
    }
}


void usage() {
    extern char *__progname;

    fprintf(stderr, "%s version %s\n" 
	"Usage: %s [-c] [-l] [-f] INTERFACE INTERFACE\n"
	    "\t-d = Dump pcap-compatible packets to stdout\n"
	    "\t-f = Run in the foreground\n"
	    "\t-h = Print this message\n"
	    "\t-m <address> = Management address (IPv4 and IPv6 supported)\n"
	    "\t-n = Use addresses specified via -m for all interfaces\n"
	    "\t-o = Run Once\n"
	    "\t-r = Receive Packets\n"
	    "\t-u <user> = Setuid User (defaults to %s)\n"
	    "\t-v = Increase logging verbosity\n"
	    "\t-z = Save received info in interface description\n"
	    "\t-c <CC> = System Country Code\n"
	    "\t-l <location> = System Location\n"
	    "\t-C = Enable CDP\n"
	    "\t-E = Enable EDP\n"
	    "\t-F = Enable FDP\n"
	    "\t-N = Enable NDP\n",
	    PACKAGE_NAME, PACKAGE_VERSION, __progname, PACKAGE_USER);

    exit(EXIT_FAILURE);
}


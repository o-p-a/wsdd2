/*
   WSDD - Web Service Dynamic Discovery protocol server

   Main file for general network handling.
  
	Copyright (c) 2016 NETGEAR
	Copyright (c) 2016 Hiro Sugawara
  
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "wsdd.h"

#include <stddef.h> // NULL
#include <stdbool.h> // bool
#include <stdio.h> // snprintf()
#include <stdlib.h> // calloc(), free(), EXIT_FAILURE
#include <stdarg.h> // va_list, va_start()
#include <setjmp.h> // jmp_buf, setjmp(), longjmp()
#include <signal.h> // sig_atomic_t, SIGHUP, SIGINT, SIGTERM
#include <string.h> // strncpy(), strchr(), strsignal()
#include <unistd.h> // gethostname()
#include <syslog.h> // openlog()
#include <limits.h> // HOST_NAME_MAX
#include <errno.h> // errno, ENOMEM
#include <err.h> // err()
#include <libgen.h> // basename()
#include <sys/select.h> // FD_SET()
#include <sys/socket.h> // SOCK_DGRAM
#include <sys/stat.h> // stat()
#include <netdb.h> // struct servent, getservbyname()
#include <arpa/inet.h> // inet_ntop()
#include <net/if.h> // if_indextoname()
#include <netinet/in.h> // IPPROTO_IP
#include <ifaddrs.h> // struct ifaddrs, getifaddrs()
#include <linux/netlink.h> // NETLINK_ROUTE
#include <linux/rtnetlink.h> // RTMGRP_LINK

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096 // PAGE_SIZE
#endif

int debug_L, debug_W, debug_N;
bool is_daemon = false;

static char *ifname = NULL;
static unsigned ifindex = 0;

static int netlink_recv(struct endpoint *ep);

static struct service services[] = {
	{
		.name	= "wsdd-mcast-v4",
		.family	= AF_INET,
		.type	= SOCK_DGRAM,
		.port_name	= "wsdd",
		.port_num	= 3702,
		.mcast_addr	= "239.255.255.250",
		.init	= wsd_init,
		.recv	= wsd_recv,
		.exit	= wsd_exit,
	},
	{
		.name	= "wsdd-mcast-v6",
		.family	= AF_INET6,
		.type	= SOCK_DGRAM,
		.port_name	= "wsdd",
		.port_num	= 3702,
		.mcast_addr	= "ff02::c",
		.init	= wsd_init,
		.recv	= wsd_recv,
		.exit	= wsd_exit,
	},
	{
		.name	= "wsdd-http-v4",
		.family	= AF_INET,
		.type	= SOCK_STREAM,
		.port_name	= "wsdd",
		.port_num	= 3702,
		.recv	= wsd_recv,
	},
	{
		.name	= "wsdd-http-v6",
		.family	= AF_INET6,
		.type	= SOCK_STREAM,
		.port_name	= "wsdd",
		.port_num	= 3702,
		.recv	= wsd_recv,
	},
	{
		.name	= "llmnr-mcast-v4",
		.family	= AF_INET,
		.type	= SOCK_DGRAM,
		.port_name	= "llmnr",
		.port_num	= 5355,
		.mcast_addr	= "224.0.0.252",
		.init	= llmnr_init,
		.recv	= llmnr_recv,
		.exit	= llmnr_exit,
	},
	{
		.name	= "llmnr-mcast-v6",
		.family	= AF_INET6,
		.type	= SOCK_DGRAM,
		.port_name	= "llmnr",
		.port_num	= 5355,
		.mcast_addr	= "ff02::1:3",
		.init	= llmnr_init,
		.recv	= llmnr_recv,
		.exit	= llmnr_exit,
	},
	{
		.name	= "llmnr-tcp-v4",
		.family	= AF_INET,
		.type	= SOCK_STREAM,
		.port_name	= "llmnr",
		.port_num	= 5355,
		.init	= llmnr_init,
		.recv	= llmnr_recv,
		.exit	= llmnr_exit,
	},
	{
		.name	= "llmnr-tcp-v6",
		.family	= AF_INET6,
		.type	= SOCK_STREAM,
		.port_name	= "llmnr",
		.port_num	= 5355,
		.init	= llmnr_init,
		.recv	= llmnr_recv,
		.exit	= llmnr_exit,
	},
	{
		.name	= "ifaddr-netlink-v4v6",
		.family	= AF_NETLINK,
		.type	= SOCK_RAW,
		.protocol	= NETLINK_ROUTE,
		.nl_groups	= RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR,
		.recv	= netlink_recv,
	},
};

/*
 * Find the interface address *ci which corresponds to received message sender address *sa
 * in order to reply with the "right" IP address.
 */

int connected_if(const _saddr_t *sa, _saddr_t *ci)
{
	int rv = -1;
	struct ifaddrs *ifaddr;

	if (getifaddrs(&ifaddr)) {
		errno = EADDRNOTAVAIL;
		return -1;
	}

	ci->ss.ss_family = sa->ss.ss_family;

	for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
		const uint8_t *_if, *_nm, *_sa;
		uint8_t *_ca;
		size_t alen;

		if (!ifa->ifa_addr || sa->ss.ss_family != ifa->ifa_addr->sa_family)
			continue;

		if (ifindex && if_nametoindex(ifa->ifa_name) != ifindex)
			continue;

		if (debug_W >= 5) {
			char name[_ADDRSTRLEN];

			if (inet_ntop(ifa->ifa_addr->sa_family,
					_SIN_ADDR((_saddr_t *)ifa->ifa_addr), name, sizeof name))
				DEBUG(4, W, "%s: %s: if=%s ", __func__, ifa->ifa_name,name);
			if (inet_ntop(sa->ss.ss_family,
					_SIN_ADDR(sa), name, sizeof name))
				DEBUG(4, W, "sc=%s ", name);
			if (inet_ntop(ifa->ifa_netmask->sa_family,
					_SIN_ADDR((_saddr_t *)ifa->ifa_netmask), name, sizeof name))
				DEBUG(4, W, "nm=%s\n", name);
		}

		switch (sa->ss.ss_family) {
		case AF_INET:
			_if = (uint8_t *) &((_saddr_t *)ifa->ifa_addr)->in.sin_addr;
			_nm = (uint8_t *) &((_saddr_t *)ifa->ifa_netmask)->in.sin_addr;
			_sa = (uint8_t *) &sa->in.sin_addr;
			_ca = (uint8_t *) &ci->in.sin_addr;
			alen = sizeof sa->in.sin_addr;
			break;
		case AF_INET6:
			_if = (uint8_t *) &((_saddr_t *)ifa->ifa_addr)->in6.sin6_addr;
			_nm = (uint8_t *) &((_saddr_t *)ifa->ifa_netmask)->in6.sin6_addr;
			_sa = (uint8_t *) &sa->in6.sin6_addr;
			_ca = (uint8_t *) &ci->in6.sin6_addr;
			alen = sizeof sa->in6.sin6_addr;
			break;
		default:
			continue;
		}

		rv = 0;
		for (size_t i = 0; i < alen; i++) {
			if ((_if[i] & _nm[i]) != (_sa[i] & _nm[i])) {
				rv = -1;
				break;
			}
		}
		if (!rv) {
			memcpy(_ca, _if, alen);
			break;
		}
	}

	if (debug_W >= 4) {
		char name[_ADDRSTRLEN];
		if (inet_ntop(ci->ss.ss_family, _SIN_ADDR(ci), name, sizeof name))
			DEBUG(4, W, "%s: ci=%s rv=%d", __func__, name, rv);
	}

	freeifaddrs(ifaddr);
	if (rv)
		errno = EADDRNOTAVAIL;
	return rv;
}

char *ip2uri(const char *ip)
{
	if (*ip == '[' || !strchr(ip, ':'))
		return strdup(ip);

	char *uri = NULL;

#if 0	/* WINDOWS 7 does not honor [xx::xx] notation. */
	asprintf(&uri, "[%s]", ip);
#else
	char name[HOST_NAME_MAX + 1];

	if (!gethostname(name, sizeof name - 1))
		uri = strdup(name);
#endif
	return uri;
}

static struct endpoint *endpoints;

static const struct sock_params {
	int family;
	const char *name;
	int ipproto_ip;
	int ip_multicast_loop;
	int ip_add_membership, ip_drop_membership;
	size_t llen, mlen, mreqlen;
} sock_params[] = {
	[AF_INET] = {
		.family	= AF_INET,
		.name	= "IPv4",
		.ipproto_ip	= IPPROTO_IP,
		.ip_multicast_loop	= IP_MULTICAST_LOOP,
		.ip_add_membership	= IP_ADD_MEMBERSHIP,
		.ip_drop_membership	= IP_DROP_MEMBERSHIP,
		.llen		= sizeof(struct sockaddr_in),
		.mlen		= sizeof(struct sockaddr_in),
		.mreqlen	= sizeof endpoints[0].mreq.ip_mreq,
	},
	[AF_INET6] = {
		.family	= AF_INET6,
		.name	= "IPv6",
		.ipproto_ip	= IPPROTO_IPV6,
		.ip_multicast_loop	= IPV6_MULTICAST_LOOP,
		.ip_add_membership	= IPV6_ADD_MEMBERSHIP,
		.ip_drop_membership	= IPV6_DROP_MEMBERSHIP,
		.llen		= sizeof(struct sockaddr_in6),
		.mlen		= sizeof(struct sockaddr_in6),
		.mreqlen	= sizeof endpoints[0].mreq.ipv6_mreq,
	},
	[AF_NETLINK] = {
		.family	= AF_NETLINK,
		.name = "NETLINK",
		.llen = sizeof(struct sockaddr_nl),
	},
};

static const char *servicename[] = {
        [SOCK_STREAM]	= "tcp",
        [SOCK_DGRAM]	= "udp",
};

static int open_ep(struct endpoint **epp, struct service *sv, const struct ifaddrs *ifa)
{
#define __FUNCTION__	"open_ep"
	struct endpoint *ep = calloc(sizeof *ep, 1);
	if (!(*epp = ep)) {
		errno = ENOMEM;
		err(EXIT_FAILURE, __FUNCTION__ ": calloc");
	}

	strncpy(ep->ifname, ifa->ifa_name, sizeof(ep->ifname));
	ep->service = sv;
	ep->family = sv->family;
	ep->type = sv->type;
	ep->protocol = sv->protocol;

	if (sv->family >= (int) ARRAY_SIZE(sock_params) || !sock_params[ep->family].name) {
		ep->errstr = __FUNCTION__ ": Unsupported address family";
		ep->_errno = EINVAL;
		return -1;
	}

	if (sv->family == AF_INET || sv->family == AF_INET6) {
		struct servent *se = getservbyname(sv->port_name, servicename[sv->type]);
		ep->port = se ? ntohs(se->s_port) : 0;
		if (!ep->port)
			ep->port = sv->port_num;
		if (!ep->port) {
			ep->errstr = __FUNCTION__ ": No port number";
			ep->_errno = EADDRNOTAVAIL;
			return -1;
		}
	}

	const struct sock_params *sp = &sock_params[ep->family];

	ep->mcast.ss.ss_family = ep->family;
	ep->mlen = sp->llen;

	ep->local.ss.ss_family = ep->family;
	ep->llen = sp->llen;

	ep->mreqlen = sp->mreqlen;

	switch (ep->family) {
	case AF_INET:
		if (sv->mcast_addr) {
			ep->mcast.in.sin_port = htons(ep->port);
			if (inet_pton(ep->family, sv->mcast_addr,
				&ep->mcast.in.sin_addr.s_addr) != 1) {
				ep->errstr = __FUNCTION__ ": Bad mcast IP addr";
				ep->_errno = errno;
				return -1;
			}
			ep->mreq.ip_mreq.imr_multiaddr = ep->mcast.in.sin_addr;
#ifdef USE_ip_mreq
			ep->mreq.ip_mreq.imr_interface = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
#else
			ep->mreq.ip_mreq.imr_address = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
			ep->mreq.ip_mreq.imr_ifindex = if_nametoindex(ep->ifname);
#endif
		}

		//ep->local.saddr_in = *(struct sockaddr_in *)ifa->ifa_addr;
		ep->local.in.sin_addr.s_addr = htonl(INADDR_ANY);
		ep->local.in.sin_port = htons(ep->port);
		break;

	case AF_INET6:
		if (sv->mcast_addr) {
			ep->mcast.in6.sin6_port = htons(ep->port);
			if (inet_pton(ep->family, sv->mcast_addr,
				ep->mcast.in6.sin6_addr.s6_addr) != 1) {
				ep->errstr = __FUNCTION__ ": Bad mcast IPv6 addr";
				ep->_errno = errno;
				return -1;
			}
			ep->mreq.ipv6_mreq.ipv6mr_multiaddr = ep->mcast.in6.sin6_addr;
			ep->mreq.ipv6_mreq.ipv6mr_interface = if_nametoindex(ep->ifname);
		}

		//ep->local.in6 = *(struct sockaddr_in6 *)ifa->ifa_addr;
		ep->local.in6.sin6_addr = in6addr_any;
		ep->local.in6.sin6_port = htons(ep->port);
		break;

	case AF_NETLINK:
		ep->local.nl.nl_groups = ep->service->nl_groups;
		break;
	}

	ep->sock = socket(ep->family, ep->type | SOCK_CLOEXEC, ep->protocol);
	if (ep->sock < 0) {
		ep->errstr = __FUNCTION__ ": Can't open socket";
		ep->_errno = errno;
		return -1;
	}

	const unsigned int enable = 1;
	setsockopt(ep->sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable);
#ifdef SO_REUSEPORT
	setsockopt(ep->sock, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof enable);
#endif
#ifdef SO_RCVBUFFORCE
	int rcvbuf = 128 * 1024;
	if ((ep->family == AF_NETLINK) &&
		setsockopt(ep->sock, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof rcvbuf)) {
		LOG(LOG_WARNING, "%s: SO_RCVBUFFORCE: %s", __FUNCTION__, strerror(errno));
	}
#endif
#ifdef IPV6_V6ONLY
	if ((ep->family == AF_INET6) &&
		setsockopt(ep->sock, sp->ipproto_ip, IPV6_V6ONLY, &enable, sizeof enable)) {
		ep->errstr = __FUNCTION__ ": IPV6_V6ONLY";
		ep->_errno = errno;
		close(ep->sock);
		return -1;
	}
#endif
#ifdef SO_BINDTODEVICE
	if (!sv->mcast_addr && (ep->family == AF_INET || ep->family == AF_INET6)) {
		struct ifreq ifr;
		strncpy(ifr.ifr_name, ep->ifname, IFNAMSIZ-1);
		if (setsockopt(ep->sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr))) {
			ep->errstr = __FUNCTION__ ": SO_BINDTODEVICE";
			ep->_errno = errno;
			close(ep->sock);
			return -1;
		}
	}
#endif

	if (bind(ep->sock, (struct sockaddr *)&ep->local, ep->llen)) {
		ep->errstr = __FUNCTION__ ": bind";
		ep->_errno = errno;
		close(ep->sock);
		ep->sock = -1;
		DEBUG(0, W, "%s: %s: %s", ep->service->name, ep->errstr, strerror(ep->_errno));
		return (ep->_errno == EADDRINUSE) ? 0 : -1;
	}

	if (sv->mcast_addr) {
		const unsigned int disable = 0, enable = 1;

#ifdef IP_PKTINFO
		if ((ep->family == AF_INET) &&
			setsockopt(ep->sock, sp->ipproto_ip, IP_PKTINFO, &enable, sizeof enable)) {
			ep->errstr = __FUNCTION__ ": IP_PKTINFO";
			ep->_errno = errno;
			close(ep->sock);
			return -1;
		}
#endif
#ifdef IP_MULTICAST_IF
		/* Set multicast sending interface to avoid error: wsdd-mcast-v4: wsd_send_soap_msg: send: No route to host */
		if ((ep->family == AF_INET) &&
			setsockopt(ep->sock, sp->ipproto_ip, IP_MULTICAST_IF, &ep->mreq, ep->mreqlen)) {
			ep->errstr = __FUNCTION__ ": IP_MULTICAST_IF";
			ep->_errno = errno;
			close(ep->sock);
			return -1;
		}
#endif
		/* Disable loopback. */
		if (setsockopt(ep->sock, sp->ipproto_ip, sp->ip_multicast_loop, &disable, sizeof disable)) {
			ep->errstr = __FUNCTION__ ": IP_MULTICAST_LOOP";
			ep->_errno = errno;
			close(ep->sock);
			return -1;
		}
		/* Set inbound multicast. */
		if (setsockopt(ep->sock, sp->ipproto_ip, sp->ip_add_membership, &ep->mreq, ep->mreqlen)) {
			ep->errstr = __FUNCTION__ ": IP_ADD_MEMBERSHIP";
			ep->_errno = errno;
			close(ep->sock);
			return -1;
		}
	}

	if (ep->type == SOCK_STREAM && listen(ep->sock, 5)) {
		ep->errstr = __FUNCTION__ ": listen";
		ep->_errno = errno;
		close(ep->sock);
		return -1;
	}

	if (ep->service->init && ep->service->init(ep)) {
		close(ep->sock);
		return -1;
	}
	return 0;
#undef __FUNCTION__
}

static void close_ep(struct endpoint *ep)
{
	if (ep->service->exit)
		ep->service->exit(ep);
	if (ep->service->mcast_addr) {
		setsockopt(ep->sock,
				sock_params[ep->family].ipproto_ip,
				sock_params[ep->family].ip_drop_membership,
				&ep->mreq, ep->mreqlen);
	}
	close(ep->sock);
}

static jmp_buf sigenv;
volatile sig_atomic_t restart;

void restart_service(void)
{
	DEBUG(1, W, "restarting service.");
	restart = 1;
	longjmp(sigenv, 1);
}

static bool is_new_addr(struct nlmsghdr *nh)
{
	struct ifaddrmsg *ifam = (struct ifaddrmsg *)NLMSG_DATA(nh);
	struct rtattr *rta = IFA_RTA(ifam);
	size_t rtasize = IFA_PAYLOAD(nh);

	if (nh->nlmsg_type != RTM_NEWADDR)
		return false;

	if (ifindex && ifam->ifa_index != ifindex) {
		char buf[IFNAMSIZ];
		if (!if_indextoname(ifindex, buf) || strcmp(buf, ifname) != 0)
			return false;
		ifindex = ifam->ifa_index;
	}

	while (RTA_OK(rta, rtasize)) {
		struct ifa_cacheinfo *cache_info;
		if (rta->rta_type == IFA_CACHEINFO) {
			cache_info = (struct ifa_cacheinfo *)(RTA_DATA(rta));
			if (cache_info->cstamp != cache_info->tstamp)
				return false;
		}
		rta = RTA_NEXT(rta, rtasize);
	}

	return true;
}

static int netlink_recv(struct endpoint *ep)
{
#define __FUNCTION__	"netlink_recv"
	char buf[PAGE_SIZE];
	struct sockaddr_nl sa;
	struct iovec iov = { buf, sizeof buf };
	struct msghdr msg = { &sa, sizeof sa, &iov, 1, NULL, 0, 0 };
	ssize_t msglen = recvmsg(ep->sock, &msg, 0);

	DEBUG(2, W, "%s: %zd bytes", __func__, msglen);
	if (msglen <= 0) {
		ep->_errno = errno;
		ep->errstr = __FUNCTION__ ": netlink_recv: recv";
		return -1;
	}

	nl_debug(buf, msglen);

	for (struct nlmsghdr *nh = (struct nlmsghdr *) buf;
			NLMSG_OK(nh, msglen) && nh->nlmsg_type != NLMSG_DONE;
			nh = NLMSG_NEXT(nh, msglen)) {
		if (is_new_addr(nh) || nh->nlmsg_type == RTM_DELADDR) {
			DEBUG(1, W, __FUNCTION__ ": address addition/change/deletion detected.");
			restart_service();
			break;
		}
	}

	return 0;
#undef __FUNCTION__
}

static void sighandler(int sig)
{
	DEBUG(0, W, "%s received.", strsignal(sig));
	switch (sig) {
	case SIGHUP:
		restart = 1;
		break;
	default:
		restart = 2;
	}
}

static void help(const char *prog, int ec, const char *fmt, ...)
{
	if (fmt) {
		va_list ap;
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
	}
	printf( "WSDD and LLMNR daemon\n"
		"Usage: %s [opts]\n"
		"       -4  IPv4 only\n"
		"       -6  IPv6 only\n"
		"       -L  LLMNR debug mode (incremental level)\n"
		"       -W  WSDD debug mode (incremental level)\n"
		"       -d  go daemon\n"
		"       -h  This message\n"
		"       -l  LLMNR only\n"
		"       -t  TCP only\n"
		"       -u  UDP only\n"
		"       -w  WSDD only\n"
		"       -i \"interface\"  Listening interface (optional)\n"
		"       -N  set NetbiosName manually\n"
		"       -G  set Workgroup manually\n"
		"       -b \"key1:val1,key2:val2,...\"  Boot parameters\n",
			prog);
	printBootInfoKeys(stdout, 11);
	exit(ec);
}

#define	_4	1
#define	_6	2
#define _TCP	1
#define _UDP	2
#define	_LLMNR	1
#define	_WSDD	2

int main(int argc, char **argv)
{
	int opt;
	const char *prog = basename(*argv);
	unsigned int ipv46 = 0, tcpudp = 0, llmnrwsdd = 0;

	while ((opt = getopt(argc, argv, "?46LWb:dhltuwi:N:G:")) != -1) {
		switch (opt) {
		case 'L':
			debug_L++;
			break;
		case 'W':
			debug_W++;
			break;
		case 'b':
			while (optarg)
				if (set_getresp(optarg, (const char **)&optarg))
					help(prog, EXIT_FAILURE,
						"bad key:val '%s'\n", optarg);
			break;
		case 'd':
			is_daemon = true;
			break;
		case 'h':
			help(prog, EXIT_SUCCESS, NULL);
			break;
		case '4':
			ipv46	|= _4;
			break;
		case '6':
			ipv46	|= _6;
			break;
		case 'l':
			llmnrwsdd |= _LLMNR;
			break;
		case 'w':
			llmnrwsdd |= _WSDD;
			break;
		case 't':
			tcpudp	|= _TCP;
			break;
		case 'u':
			tcpudp	|= _UDP;
			break;
		case 'i':
			if (optarg != NULL && strlen(optarg) > 1) {
				ifindex = if_nametoindex(optarg);
				if (ifindex == 0)
					help(prog, EXIT_FAILURE, "bad interface '%s'\n", optarg);
				ifname = strdup(optarg);
			}
			break;
		case 'N':
			if (optarg != NULL && strlen(optarg) > 1) {
				netbiosname = strdup(optarg);
			}
			break;
		case 'G':
			if (optarg != NULL && strlen(optarg) > 1) {
				workgroup = strdup(optarg);
			}
			break;
		case '?':
			if (optopt == 'b' || optopt == 'i' || optopt == 'N' || optopt == 'G')
				fprintf(stderr, "Option -%c requires an argument.\n", optopt);
			/* ... fall through ... */
		default:
			help(prog, EXIT_FAILURE, "bad option '%c'\n", opt);
		}
	}

	if (!ipv46)
		ipv46	= _4 | _6;
	if (!llmnrwsdd)
		llmnrwsdd = _LLMNR | _WSDD;
	if (!tcpudp)
		tcpudp	= _TCP | _UDP;

	if (is_daemon) {
		pid_t pid = fork();

		if (pid < 0)
			err(EXIT_FAILURE, "fork");
		if (pid)
			exit(EXIT_SUCCESS);
	}

	openlog(prog, LOG_PID, LOG_USER);
	LOG(LOG_INFO, "starting.");

again:
	{}	/* Necessary to satisfy C syntax for statement labeling. */
	struct sigaction sigact, oldact;

	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigact.sa_handler = sighandler;

	if (sigaction(SIGINT, &sigact, &oldact) ||
		sigaction(SIGHUP, &sigact, &oldact) ||
		sigaction(SIGTERM, &sigact, &oldact)) {
		err(EXIT_FAILURE, "cannot install signal handler.");
	}

	struct ifaddrs *ifaddrs;
	fd_set fds;
	int rv = 0, nfds = -1;
	struct endpoint *ep, *badep = NULL;

	FD_ZERO(&fds);

	if (getifaddrs(&ifaddrs) != 0)
		err(EXIT_FAILURE, "ifaddrs");

	for (size_t svn = 0; svn < ARRAY_SIZE(services); svn++) {
		struct service *sv = &services[svn];

		if (!(ipv46 & _4) && sv->family == AF_INET)
			continue;
		if (!(ipv46 & _6) && sv->family == AF_INET6)
			continue;
		if (!(tcpudp & _TCP) && sv->type == SOCK_STREAM)
			continue;
		if (!(tcpudp & _UDP) && sv->type == SOCK_DGRAM)
			continue;
		if (!(llmnrwsdd & _LLMNR) && strstr(sv->name, "llmnr"))
			continue;
		if (!(llmnrwsdd & _WSDD) && strstr(sv->name, "wsdd"))
			continue;

		if (sv->family == AF_INET || sv->family == AF_INET6) {
			for (struct ifaddrs *ifa = ifaddrs; ifa; ifa = ifa->ifa_next) {
				if (!ifa->ifa_addr ||
					(ifa->ifa_addr->sa_family != sv->family) ||
					(ifa->ifa_flags & IFF_LOOPBACK) ||
					(ifa->ifa_flags & IFF_SLAVE) ||
					(ifname && strcmp(ifa->ifa_name, ifname) != 0) ||
					(!strcmp(ifa->ifa_name, "LeafNets")) ||
					(!strncmp(ifa->ifa_name, "docker", 6)) ||
					(!strncmp(ifa->ifa_name, "veth", 4)) ||
					(!strncmp(ifa->ifa_name, "tun", 3)) ||
					(!strncmp(ifa->ifa_name, "ppp", 3)) ||
					(!strncmp(ifa->ifa_name, "zt", 2)) ||
					(sv->mcast_addr && !(ifa->ifa_flags & IFF_MULTICAST)))
					continue;

				// skip bridge ports unless it specified on command line
				if (!ifname) {
					struct stat st;
					char path[sizeof("/sys/class/net//brport") + IFNAMSIZ];
					snprintf(path, sizeof(path), "/sys/class/net/%s/brport", ifa->ifa_name);
					if (stat(path, &st) == 0)
						continue;
				}

				char ifaddr[_ADDRSTRLEN];
				void *addr = _SIN_ADDR((_saddr_t *)ifa->ifa_addr);

				inet_ntop(ifa->ifa_addr->sa_family, addr, ifaddr, sizeof(ifaddr));
				DEBUG(2, W, "%s %s %s %s:%d @ %s", sv->name, servicename[sv->type],
					sv->mcast_addr ? sv->mcast_addr : "-",
					ifaddr, sv->port_num, ifa->ifa_name);

				if (open_ep(&ep, sv, ifa)) {
					LOG(LOG_ERR, "error: %s: %s: %s",
						ep->service->name, ep->errstr, strerror(ep->_errno));
					free(ep);
					continue;
				} else if (ep->sock < 0) {
					free(ep);
				} else {
					ep->next = endpoints;
					endpoints = ep;
					FD_SET(ep->sock, &fds);
					if (nfds < ep->sock)
						nfds = ep->sock;
				}
			}
			if (badep)
				break;

		} else if (sv->family == AF_NETLINK) {
			const struct ifaddrs ifa = { .ifa_name = "netlink", };

			DEBUG(2, W, "%s 0x%x @ %s", sv->name, sv->nl_groups, ifa.ifa_name);
			if (open_ep(&ep, sv, &ifa)) {
				badep = ep;
				break;
			} else if (ep->sock < 0) {
				free(ep);
			} else {
				ep->next = endpoints;
				endpoints = ep;
				FD_SET(ep->sock, &fds);
				if (nfds < ep->sock)
					nfds = ep->sock;
			}
		}
	}

	freeifaddrs(ifaddrs);

	if (!badep) {
		int n = 0;

		if (setjmp(sigenv))
			goto end;

		do {
			fd_set rfds = fds;
			n = select(nfds + 1, &rfds, NULL, NULL, NULL);
			DEBUG(3, W, "select: n=%d", n);
			for (ep = endpoints; n > 0 && ep; ep = ep->next) {
				if (!FD_ISSET(ep->sock, &rfds))
					continue;
				DEBUG(3, W, "dispatch %s_recv", ep->service->name);
				n--;
				if (ep->service->recv) {
					int ret = ep->service->recv(ep);
					if (ret < 0) {
						DEBUG(1, W, "Detected %s socket error, restarting",
							ep->service->name);
						restart_service();
					}
				}
			}
		} while (n >= 0 && !restart);

		if (n < 0 && errno != EINTR) {
			LOG(LOG_WARNING, "%s: select: %s", __func__, strerror(errno));
			rv = EXIT_FAILURE;
		}
	}
end:
	{}
	const char *badservice = NULL, *badbad = NULL;
	int baderrno = 0;

	if (badep) {
		badservice = badep->service->name ? badep->service->name : "";
		badbad = badep->errstr ? badep->errstr : "";
		baderrno = badep->_errno;
	}

	while (endpoints) {
		struct endpoint *tempep = endpoints->next;
		close_ep(endpoints);
		free(endpoints);
		endpoints = tempep;
	}

	if (badep) {
		LOG(LOG_ERR, "%s: %s: terminating.", badservice, badbad);
		closelog();
		errno = baderrno;
		err(EXIT_FAILURE, "%s: %s", badservice, badbad);
	}

	if (restart == 1) {
		restart = 0;
		goto again;
	}

	LOG(LOG_INFO, "terminating.");
	closelog();
	return rv;
}

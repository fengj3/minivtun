/*
 * Copyright (c) 2015 Justin Liu
 * Author: Justin Liu <rssnsj@gmail.com>
 * https://github.com/rssnsj/minivtun
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ether.h>
#include <linux/if.h>
#include <linux/if_tun.h>

#include "minivtun.h"

unsigned g_keepalive_timeo = 13;
unsigned g_reconnect_timeo = 60;
const char *g_pid_file = NULL;
const char *g_crypto_passwd = "";
char g_crypto_passwd_md5sum[16];
AES_KEY g_encrypt_key;
AES_KEY g_decrypt_key;
struct in_addr g_local_tun_in;
struct in6_addr g_local_tun_in6;

char g_devname[40];
static unsigned g_tun_mtu = 1416;
bool g_in_background = false;

static struct option long_opts[] = {
	{ "local", required_argument, 0, 'l', },
	{ "remote", required_argument, 0, 'r', },
	{ "ipv4-addr", required_argument, 0, 'a', },
	{ "ipv6-addr", required_argument, 0, 'A', },
	{ "mtu", required_argument, 0, 'm', },
	{ "keepalive", required_argument, 0, 't', },
	{ "ifname", required_argument, 0, 'n', },
	{ "pidfile", required_argument, 0, 'p', },
	{ "encryption-key", required_argument, 0, 'e', },
	{ "no-encryption", no_argument, 0, 'N', },
	{ "route", required_argument, 0, 'v', },
	{ "daemon", no_argument, 0, 'd', },
	{ "help", no_argument, 0, 'h', },
	{ 0, 0, 0, 0, },
};

static void print_help(int argc, char *argv[])
{
	printf("Mini virtual tunneller in non-standard protocol.\n");
	printf("Usage:\n");
	printf("  %s [options]\n", argv[0]);
	printf("Options:\n");
	printf("  -l, --local <ip:port>               IP:port for server to listen\n");
	printf("  -r, --remote <ip:port>              IP:port of server to connect\n");
	printf("  -a, --ipv4-addr <tun_lip/tun_rip>   pointopoint IPv4 pair of the virtual interface\n");
	printf("                  <tun_lip/pfx_len>   IPv4 address/prefix length pair\n");
	printf("  -A, --ipv6-addr <tun_ip6/pfx_len>   IPv6 address/prefix length pair\n");
	printf("  -m, --mtu <mtu>                     set MTU size, default: %u.\n", g_tun_mtu);
	printf("  -t, --keepalive <keepalive_timeo>   interval of keep-alive packets, default: %u\n", g_keepalive_timeo);
	printf("  -n, --ifname <ifname>               virtual interface name\n");
	printf("  -p, --pidfile <pid_file>            PID file of the daemon\n");
	printf("  -e, --encryption-key <encrypt_key>  shared password for data encryption\n");
	printf("  -v, --route <network/prefix=gateway>\n");
	printf("                                      route a network to a client address, can be multiple\n");
	printf("  -N, --no-encryption                 turn off encryption for tunnelling data\n");
	printf("  -d, --daemon                        run as daemon process\n");
	printf("  -h, --help                          print this help\n");
}

static int tun_alloc(char *dev)
{
	struct ifreq ifr;
	int fd, err;

	if ((fd = open("/dev/net/tun", O_RDWR)) < 0) {
		if ((fd = open("/dev/tun", O_RDWR)) < 0)
			return -1;
	}

	memset(&ifr, 0, sizeof(ifr));
	/* Flags: IFF_TUN   - TUN device (no Ethernet headers)
	 *        IFF_TAP   - TAP device
	 *
	 *        IFF_NO_PI - Do not provide packet information
	 */
	ifr.ifr_flags = IFF_TUN;
	if (*dev)
		strncpy(ifr.ifr_name, dev, IFNAMSIZ);

	if ((err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0){
		close(fd);
		return err;
	}
	strcpy(dev, ifr.ifr_name);
	return fd;
}

static void parse_virtual_route(const char *arg)
{
	char expr[80], *net, *pfx, *gw;
	struct in_addr network, gateway;
	unsigned prefix = 0;

	strncpy(expr, arg, sizeof(expr));
	expr[sizeof(expr) - 1] = '\0';

	/* 192.168.0.0/16=10.7.0.1 */
	net = expr;
	if ((pfx = strchr(net, '/')) == NULL) {
		fprintf(stderr, "*** Not a valid route expression '%s'.\n", arg);
		exit(1);
	}
	*(pfx++) = '\0';
	if ((gw = strchr(pfx, '=')) == NULL) {
		fprintf(stderr, "*** Not a valid route expression '%s'.\n", arg);
		exit(1);
	}
	*(gw++) = '\0';

	if (!inet_pton(AF_INET, net, &network) ||
		!inet_pton(AF_INET, gw, &gateway) || sscanf(pfx, "%u", &prefix) != 1) {
		fprintf(stderr, "*** Not a valid route expression '%s'.\n", arg);
		exit(1);
	}

	vt_route_add(&network, prefix, &gateway);
}

int main(int argc, char *argv[])
{
	const char *tun_ip_config = NULL, *tun_ip6_config = NULL;
	const char *loc_addr_pair = NULL;
	const char *peer_addr_pair = NULL;
	char cmd[100];
	int tunfd, opt;

	while ((opt = getopt_long(argc, argv, "r:l:a:A:m:t:n:p:e:v:Ndh",
			long_opts, NULL)) != -1) {

		switch (opt) {
		case 'l':
			loc_addr_pair = optarg;
			break;
		case 'r':
			peer_addr_pair = optarg;
			break;
		case 'a':
			tun_ip_config = optarg;
			break;
		case 'A':
			tun_ip6_config = optarg;
			break;
		case 'm':
			g_tun_mtu = (unsigned)strtoul(optarg, NULL, 10);
			break;
		case 't':
			g_keepalive_timeo = (unsigned)strtoul(optarg, NULL, 10);
			break;
		case 'n':
			strncpy(g_devname, optarg, sizeof(g_devname) - 1);
			g_devname[sizeof(g_devname) - 1] = '\0';
			break;
		case 'p':
			g_pid_file = optarg;
			break;
		case 'e':
			g_crypto_passwd = optarg;
			break;
		case 'v':
			parse_virtual_route(optarg);
			break;
		case 'N':
			g_crypto_passwd = NULL;
			break;
		case 'd':
			g_in_background = true;
			break;
		case 'h':
			print_help(argc, argv);
			exit(0);
			break;
		case '?':
			exit(1);
		}
	}

	if (strlen(g_devname) == 0)
		strcpy(g_devname, "mv%d");
	if ((tunfd = tun_alloc(g_devname)) < 0) {
		fprintf(stderr, "*** open_tun() failed: %s.\n", strerror(errno));
		exit(1);
	}

	/* Configure IPv4 address for the interface. */
	if (tun_ip_config) {
		char s_lip[20], s_rip[20], *sp;
		struct in_addr vaddr;
		int na = 0;

		if (!(sp = strchr(tun_ip_config, '/'))) {
			fprintf(stderr, "*** Invalid IPv4 address pair: %s.\n", tun_ip_config);
			exit(1);
		}
		strncpy(s_lip, tun_ip_config, sp - tun_ip_config);
		s_lip[sp - tun_ip_config] = '\0';
		sp++;
		strncpy(s_rip, sp, sizeof(s_rip));
		s_rip[sizeof(s_rip) - 1] = '\0';

		if (!inet_pton(AF_INET, s_lip, &vaddr)) {
			fprintf(stderr, "*** Invalid local IPv4 address: %s.\n", s_lip);
			exit(1);
		}
		g_local_tun_in = vaddr;
		if (inet_pton(AF_INET, s_rip, &vaddr)) {
			struct in_addr __network = { .s_addr = 0 };
			sprintf(cmd, "ifconfig %s %s pointopoint %s", g_devname, s_lip, s_rip);
			vt_route_add(&__network, 0, &vaddr);
		} else if (sscanf(s_rip, "%d", &na) == 1 && na > 0 && na < 31 ) {
			uint32_t mask = ~((1 << (32 - na)) - 1);
			sprintf(s_rip, "%u.%u.%u.%u", mask >> 24, (mask >> 16) & 0xff,
					(mask >> 8) & 0xff, mask & 0xff);
			sprintf(cmd, "ifconfig %s %s netmask %s", g_devname, s_lip, s_rip);
		} else {
			fprintf(stderr, "*** Not a legal netmask or prefix length: %s.\n",
					s_rip);
			exit(1);
		}
		(void)system(cmd);
	}

	/* Configure IPv6 address if set. */
	if (tun_ip6_config) {
		char s_lip[50], s_pfx[20], *sp;
		struct in6_addr vaddr;
		int pfx_len = 0;

		if (!(sp = strchr(tun_ip6_config, '/'))) {
			fprintf(stderr, "*** Invalid IPv6 address pair: %s.\n", tun_ip6_config);
			exit(1);
		}
		strncpy(s_lip, tun_ip6_config, sp - tun_ip6_config);
		s_lip[sp - tun_ip6_config] = '\0';
		sp++;
		strncpy(s_pfx, sp, sizeof(s_pfx));
		s_pfx[sizeof(s_pfx) - 1] = '\0';

		if (!inet_pton(AF_INET6, s_lip, &vaddr)) {
			fprintf(stderr, "*** Invalid local IPv6 address: %s.\n", s_lip);
			exit(1);
		}
		g_local_tun_in6 = vaddr;
		if (!(sscanf(s_pfx, "%d", &pfx_len) == 1 && pfx_len > 0 && pfx_len <= 128)) {
			fprintf(stderr, "*** Not a legal prefix length: %s.\n", s_pfx);
			exit(1);
		}

		sprintf(cmd, "ifconfig %s add %s/%d", g_devname, s_lip, pfx_len);
		(void)system(cmd);
	}

	/* Always bring it up with proper MTU size. */
	sprintf(cmd, "ifconfig %s mtu %u; ifconfig %s up", g_devname, g_tun_mtu, g_devname);
	(void)system(cmd);

	if (g_crypto_passwd) {
		gen_encrypt_key(&g_encrypt_key, g_crypto_passwd);
		gen_decrypt_key(&g_decrypt_key, g_crypto_passwd);
		gen_string_md5sum(g_crypto_passwd_md5sum, g_crypto_passwd);
	} else {
		fprintf(stderr, "*** WARNING: Transmission will not be encrypted.\n");
	}

	if (loc_addr_pair) {
		run_server(tunfd, loc_addr_pair);
	} else if (peer_addr_pair) {
		run_client(tunfd, peer_addr_pair);
	} else {
		fprintf(stderr, "*** No valid local or peer address specified.\n");
		exit(1);
	}

	return 0;
}


/*
 * Dreamcast/KOS DNS resolver shim.
 *
 * Flycast 2.5 on Windows can provide a working BBA link + DHCP, but KOS/newlib
 * DNS resolution may not be functional in that environment. NetSurf uses
 * libcurl, which in turn relies on libc getaddrinfo/gethostbyname.
 *
 * We keep this isolated to the Dreamcast frontend by using the linker
 * --wrap mechanism to intercept resolver calls and implement a minimal IPv4
 * UDP DNS client using the DNS server address configured by KOS.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <kos.h>

/* Some libcs don't declare these if DNS isn't fully supported. */
#ifndef HOST_NOT_FOUND
#define HOST_NOT_FOUND 1
#endif

#ifndef AF_UNSPEC
#define AF_UNSPEC 0
#endif

#ifndef AI_PASSIVE
struct addrinfo {
	int ai_flags;
	int ai_family;
	int ai_socktype;
	int ai_protocol;
	size_t ai_addrlen;
	struct sockaddr *ai_addr;
	char *ai_canonname;
	struct addrinfo *ai_next;
};
#endif

#ifndef EAI_NONAME
#define EAI_NONAME 8
#endif
#ifndef EAI_FAIL
#define EAI_FAIL 4
#endif
#ifndef EAI_FAMILY
#define EAI_FAMILY 1
#endif
#ifndef EAI_SERVICE
#define EAI_SERVICE 9
#endif

/* -------------------------------------------------------------------------- */
/* Minimal UDP DNS client (A record only) */
/* -------------------------------------------------------------------------- */

#define DC_DNS_PORT 53
#define DC_DNS_MAX_PACKET 512

static uint16_t dc_dns_next_id;

static int
is_digit_str(const char *s)
{
	if (s == NULL || *s == '\0')
		return 0;
	for (; *s != '\0'; s++) {
		if (*s < '0' || *s > '9')
			return 0;
	}
	return 1;
}

static uint16_t
service_to_port(const char *service)
{
	if (service == NULL || *service == '\0')
		return 0;

	if (is_digit_str(service)) {
		long v = strtol(service, NULL, 10);
		if (v < 0 || v > 65535)
			return 0;
		return (uint16_t)v;
	}

	if (strcasecmp(service, "http") == 0)
		return 80;
	if (strcasecmp(service, "https") == 0)
		return 443;

	return 0;
}

static int
dc_dns_build_query(uint8_t *out, size_t out_len, uint16_t id, const char *host)
{
	/* DNS header is 12 bytes */
	size_t off = 0;
	const char *p = host;

	if (out_len < 12)
		return -1;

	/* ID */
	out[off++] = (uint8_t)(id >> 8);
	out[off++] = (uint8_t)(id & 0xff);
	/* Flags: recursion desired */
	out[off++] = 0x01;
	out[off++] = 0x00;
	/* QDCOUNT=1 */
	out[off++] = 0x00;
	out[off++] = 0x01;
	/* ANCOUNT/NSCOUNT/ARCOUNT=0 */
	out[off++] = 0x00;
	out[off++] = 0x00;
	out[off++] = 0x00;
	out[off++] = 0x00;
	out[off++] = 0x00;
	out[off++] = 0x00;

	/* QNAME */
	while (*p != '\0') {
		const char *dot = strchr(p, '.');
		size_t labellen = dot ? (size_t)(dot - p) : strlen(p);

		if (labellen == 0 || labellen > 63)
			return -1;
		if (off + 1 + labellen + 1 + 4 > out_len)
			return -1;

		out[off++] = (uint8_t)labellen;
		memcpy(out + off, p, labellen);
		off += labellen;

		if (!dot)
			break;
		p = dot + 1;
	}
	out[off++] = 0x00; /* terminator */

	/* QTYPE=A (1), QCLASS=IN (1) */
	out[off++] = 0x00;
	out[off++] = 0x01;
	out[off++] = 0x00;
	out[off++] = 0x01;

	return (int)off;
}

static int
dc_dns_skip_name(const uint8_t *msg, size_t msg_len, size_t *off_io)
{
	size_t off = *off_io;
	int max_steps = 64;

	while (off < msg_len && max_steps-- > 0) {
		uint8_t c = msg[off];

		if (c == 0) {
			off++;
			*off_io = off;
			return 0;
		}

		/* compression pointer */
		if ((c & 0xC0) == 0xC0) {
			if (off + 1 >= msg_len)
				return -1;
			off += 2;
			/* pointer terminates the name */
			*off_io = off;
			return 0;
		}

		/* label */
		if (c > 63)
			return -1;
		if (off + 1 + c > msg_len)
			return -1;
		off += 1 + c;
	}

	return -1;
}

static int
dc_dns_query_A(const char *host, struct in_addr *out_addr)
{
	uint8_t pkt[DC_DNS_MAX_PACKET];
	int pkt_len;
	int sock;
	struct sockaddr_in sa;
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);
	fd_set rfds;
	struct timeval tv;
	int sel;
	ssize_t r;
	size_t off;
	uint16_t id;
	uint16_t flags;
	uint16_t qdcount, ancount;
	uint16_t i;

	printf("[DNS] dc_dns_query_A: resolving '%s'\n", host ? host : "(null)");

	if (net_default_dev == NULL) {
		printf("[DNS] ERROR: net_default_dev is NULL\n");
		return -1;
	}

	if (net_default_dev->dns[0] == 0 && net_default_dev->dns[1] == 0 &&
	    net_default_dev->dns[2] == 0 && net_default_dev->dns[3] == 0) {
		printf("[DNS] ERROR: DNS server is 0.0.0.0\n");
		return -1;
	}

	printf("[DNS] Using DNS server: %d.%d.%d.%d\n",
	       net_default_dev->dns[0], net_default_dev->dns[1],
	       net_default_dev->dns[2], net_default_dev->dns[3]);

	id = ++dc_dns_next_id;
	pkt_len = dc_dns_build_query(pkt, sizeof(pkt), id, host);
	if (pkt_len < 0)
		return -1;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		printf("[DNS] ERROR: socket() failed, errno=%d\n", errno);
		return -1;
	}
	printf("[DNS] Socket created: fd=%d\n", sock);

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(DC_DNS_PORT);
	memcpy(&sa.sin_addr.s_addr, net_default_dev->dns, 4);

	if (sendto(sock, pkt, (size_t)pkt_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("[DNS] ERROR: sendto() failed, errno=%d\n", errno);
		close(sock);
		return -1;
	}
	printf("[DNS] Query sent (%d bytes)\n", pkt_len);

	FD_ZERO(&rfds);
	FD_SET(sock, &rfds);
	tv.tv_sec = 2;
	tv.tv_usec = 0;
	sel = select(sock + 1, &rfds, NULL, NULL, &tv);
	if (sel <= 0) {
		printf("[DNS] ERROR: select() returned %d (timeout or error)\n", sel);
		close(sock);
		return -1;
	}

	r = recvfrom(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fromlen);
	close(sock);
	printf("[DNS] Received %zd bytes\n", r);
	if (r < 12) {
		printf("[DNS] ERROR: response too short\n");
		return -1;
	}

	/* header */
	if (((uint16_t)pkt[0] << 8 | pkt[1]) != id)
		return -1;
	flags = (uint16_t)pkt[2] << 8 | pkt[3];
	/* rcode in low 4 bits */
	if ((flags & 0x000F) != 0)
		return -1;

	qdcount = (uint16_t)pkt[4] << 8 | pkt[5];
	ancount = (uint16_t)pkt[6] << 8 | pkt[7];

	off = 12;
	for (i = 0; i < qdcount; i++) {
		if (dc_dns_skip_name(pkt, (size_t)r, &off) < 0)
			return -1;
		if (off + 4 > (size_t)r)
			return -1;
		off += 4; /* qtype + qclass */
	}

	for (i = 0; i < ancount; i++) {
		uint16_t type, class_, rdlen;

		if (dc_dns_skip_name(pkt, (size_t)r, &off) < 0)
			return -1;
		if (off + 10 > (size_t)r)
			return -1;

		type = (uint16_t)pkt[off] << 8 | pkt[off + 1];
		class_ = (uint16_t)pkt[off + 2] << 8 | pkt[off + 3];
		off += 8; /* type + class + ttl */
		rdlen = (uint16_t)pkt[off] << 8 | pkt[off + 1];
		off += 2;

		if (off + rdlen > (size_t)r)
			return -1;

		if (type == 1 && class_ == 1 && rdlen == 4) {
			memcpy(&out_addr->s_addr, pkt + off, 4);
			return 0;
		}

		off += rdlen;
	}

	return -1;
}

/* -------------------------------------------------------------------------- */
/* Resolver API shims (via --wrap) */
/* -------------------------------------------------------------------------- */

static int
dc_getaddrinfo_impl(const char *node, const char *service,
			const struct addrinfo *hints, struct addrinfo **res)
{
	struct addrinfo *ai;
	struct sockaddr_in *sin;
	struct in_addr addr;
	int family = AF_INET;
	uint16_t port;

	if (res == NULL)
		return EAI_FAIL;
	*res = NULL;

	if (hints != NULL && hints->ai_family != AF_UNSPEC) {
		family = hints->ai_family;
	}
	if (family != AF_INET)
		return EAI_FAMILY;

	port = service_to_port(service);
	if (service != NULL && port == 0)
		return EAI_SERVICE;

	if (node == NULL) {
		addr.s_addr = htonl(INADDR_ANY);
	}
	else if (inet_aton(node, &addr) != 0) {
		/* numeric host */
	}
	else {
		if (dc_dns_query_A(node, &addr) < 0)
			return EAI_NONAME;
	}

	ai = (struct addrinfo *)calloc(1, sizeof(*ai));
	sin = (struct sockaddr_in *)calloc(1, sizeof(*sin));
	if (ai == NULL || sin == NULL) {
		free(ai);
		free(sin);
		return EAI_FAIL;
	}

	sin->sin_family = AF_INET;
	sin->sin_port = htons(port);
	sin->sin_addr = addr;

	ai->ai_family = AF_INET;
	ai->ai_socktype = (hints != NULL) ? hints->ai_socktype : 0;
	ai->ai_protocol = (hints != NULL) ? hints->ai_protocol : 0;
	ai->ai_addrlen = sizeof(*sin);
	ai->ai_addr = (struct sockaddr *)sin;
	ai->ai_next = NULL;

	*res = ai;
	return 0;
}

static void
dc_freeaddrinfo_impl(struct addrinfo *ai)
{
	while (ai != NULL) {
		struct addrinfo *next = ai->ai_next;
		free(ai->ai_addr);
		free(ai->ai_canonname);
		free(ai);
		ai = next;
	}
}

static const char *
dc_gai_strerror_impl(int errcode)
{
	switch (errcode) {
	case 0: return "success";
	case EAI_NONAME: return "name or service not known";
	case EAI_FAIL: return "non-recoverable failure";
	case EAI_FAMILY: return "ai_family not supported";
	case EAI_SERVICE: return "service not supported";
	default: return "unknown error";
	}
}

/*
 * netdb.h provides this on KOS/newlib. We update it so callers can inspect
 * resolver failures.
 */

static struct hostent dc_he;
static char *dc_he_aliases[1];
static char *dc_he_addr_list[2];
static struct in_addr dc_he_addr;

static struct hostent *
dc_gethostbyname_impl(const char *name)
{
	if (name == NULL || *name == '\0') {
		h_errno = HOST_NOT_FOUND;
		return NULL;
	}

	if (inet_aton(name, &dc_he_addr) == 0) {
		if (dc_dns_query_A(name, &dc_he_addr) < 0) {
			h_errno = HOST_NOT_FOUND;
			return NULL;
		}
	}

	memset(&dc_he, 0, sizeof(dc_he));
	dc_he.h_name = (char *)name;
	dc_he_aliases[0] = NULL;
	dc_he.h_aliases = dc_he_aliases;
	dc_he.h_addrtype = AF_INET;
	dc_he.h_length = sizeof(dc_he_addr);
	dc_he_addr_list[0] = (char *)&dc_he_addr;
	dc_he_addr_list[1] = NULL;
	dc_he.h_addr_list = dc_he_addr_list;

	h_errno = 0;
	return &dc_he;
}

/* Wrapped entry points (linker: -Wl,--wrap=<symbol>) */
int __wrap_getaddrinfo(const char *node, const char *service,
			const struct addrinfo *hints, struct addrinfo **res);
void __wrap_freeaddrinfo(struct addrinfo *ai);
const char *__wrap_gai_strerror(int errcode);
struct hostent *__wrap_gethostbyname(const char *name);

/*
 * These are the actual wrapped entry points used when linking with
 * -Wl,--wrap=<symbol>.
 */
int __wrap_getaddrinfo(const char *node, const char *service,
			const struct addrinfo *hints, struct addrinfo **res)
{
	int ret;
	printf("[DNS] __wrap_getaddrinfo called: node='%s', service='%s'\n",
	       node ? node : "(null)", service ? service : "(null)");
	ret = dc_getaddrinfo_impl(node, service, hints, res);
	printf("[DNS] __wrap_getaddrinfo returning %d\n", ret);
	return ret;
}

void __wrap_freeaddrinfo(struct addrinfo *ai)
{
	dc_freeaddrinfo_impl(ai);
}

const char *__wrap_gai_strerror(int errcode)
{
	return dc_gai_strerror_impl(errcode);
}

struct hostent *__wrap_gethostbyname(const char *name)
{
	printf("[DNS] __wrap_gethostbyname called: name='%s'\n", name ? name : "(null)");
	return dc_gethostbyname_impl(name);
}

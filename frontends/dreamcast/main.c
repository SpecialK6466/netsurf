/*
 * Dreamcast frontend entry point.
 *
 * This is a thin wrapper which allows the Dreamcast build to provide its own
 * main() while reusing the framebuffer frontend implementation.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <kos.h>

#include "dreamcast/sdl_dc_surface.h"

/* The Dreamcast build uses -Dmain=framebuffer_main to rename the framebuffer
 * frontend's main() implementation. That flag also affects this file, so
 * undo it here to keep a real entry point named main().
 */
#ifdef main
#undef main
#endif

/* Provided by framebuffer/gui.c via -Dmain=framebuffer_main */
int framebuffer_main(int argc, char **argv);

static const char *
dc_eai_str(int rc)
{
	/* netdb.h provides these, but not necessarily gai_strerror(). */
	switch (rc) {
	case 0:
		return "success";
	case EAI_AGAIN:
		return "try again";
	case EAI_FAIL:
		return "non-recoverable failure";
	case EAI_FAMILY:
		return "invalid family";
	case EAI_NONAME:
		return "name not found";
	case EAI_SERVICE:
		return "invalid service";
	case EAI_SOCKTYPE:
		return "invalid socktype";
	case EAI_MEMORY:
		return "out of memory";
	default:
		return "unknown";
	}
}

static void
dreamcast_net_init(void)
{
#if defined(NETSURF_DC_ENABLE_NET) && (NETSURF_DC_ENABLE_NET != 0)
	/*
	 * Bring up KOS networking.
	 *
	 * KOS_INIT_FLAGS(INIT_NET) initializes the stack, but you still need to call
	 * net_init() to configure the default interface (DHCP or flashrom settings).
	 */
	int rc;

	fprintf(stderr, "[dcnet] net_init(0) starting...\n");
	rc = net_init(0);
	if (rc < 0) {
		fprintf(stderr, "[dcnet] net_init failed: %d\n", rc);
		return;
	}

	if (net_default_dev == NULL) {
		fprintf(stderr, "[dcnet] net_default_dev is NULL after net_init\n");
		return;
	}

	fprintf(stderr,
		"[dcnet] if=%s%d ip=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u\n",
		net_default_dev->name ? net_default_dev->name : "?",
		net_default_dev->index,
		net_default_dev->ip_addr[0], net_default_dev->ip_addr[1],
		net_default_dev->ip_addr[2], net_default_dev->ip_addr[3],
		net_default_dev->gateway[0], net_default_dev->gateway[1],
		net_default_dev->gateway[2], net_default_dev->gateway[3],
		net_default_dev->dns[0], net_default_dev->dns[1],
		net_default_dev->dns[2], net_default_dev->dns[3]);

	/*
	 * DNS self-test: this makes it obvious in logs whether hostname resolution is
	 * working (either via KOS/newlib resolver or the Dreamcast DNS shim).
	 */
	if (!(net_default_dev->dns[0] == 0 && net_default_dev->dns[1] == 0 &&
	      net_default_dev->dns[2] == 0 && net_default_dev->dns[3] == 0)) {
		struct addrinfo hints;
		struct addrinfo *res = NULL;
		int gai_rc;
		char ipbuf[INET_ADDRSTRLEN];

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;

		gai_rc = getaddrinfo("dns.flyca.st", "80", &hints, &res);
		if (gai_rc != 0 || res == NULL || res->ai_addr == NULL) {
			fprintf(stderr, "[dcnet] dns test failed: rc=%d (%s)\n",
				gai_rc, dc_eai_str(gai_rc));
		} else {
			struct sockaddr_in sin;
			const char *ip;

			memset(&sin, 0, sizeof(sin));
			if (res->ai_addrlen >= (socklen_t)sizeof(sin)) {
				memcpy(&sin, res->ai_addr, sizeof(sin));
			} else {
				memcpy(&sin, res->ai_addr, res->ai_addrlen);
			}
			ip = inet_ntop(AF_INET, &sin.sin_addr, ipbuf, sizeof(ipbuf));
			fprintf(stderr, "[dcnet] dns.flyca.st -> %s\n", (ip != NULL) ? ip : "?");
		}
		if (res != NULL)
			freeaddrinfo(res);
	}
#endif
}

int
main(int argc, char **argv)
{
	dreamcast_net_init();

	/* Run the (renamed) framebuffer frontend main loop. */
	return framebuffer_main(argc, argv);
}

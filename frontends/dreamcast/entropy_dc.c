/*
 * Dreamcast entropy and network I/O support for mbedTLS.
 *
 * Provides:
 * - mbedtls_hardware_poll() for entropy (MBEDTLS_ENTROPY_HARDWARE_ALT)
 * - mbedtls_net_send/recv for socket I/O (since MBEDTLS_NET_C is disabled)
 *
 * The Dreamcast lacks a hardware RNG. This implementation uses the system
 * timer and other varying state to generate pseudo-random entropy. While not
 * cryptographically ideal, it provides sufficient randomness for TLS session
 * keys in a testing/hobbyist context.
 */

#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <sys/socket.h>

#ifdef __DREAMCAST__
#include <arch/timer.h>
#include <dc/maple.h>
#endif

/* mbedTLS error codes for network operations */
#define MBEDTLS_ERR_NET_SEND_FAILED    -0x004E
#define MBEDTLS_ERR_NET_RECV_FAILED    -0x004C
#define MBEDTLS_ERR_SSL_WANT_READ      -0x6900
#define MBEDTLS_ERR_SSL_WANT_WRITE     -0x6880

/*
 * mbedtls_hardware_poll - entropy source callback for mbedTLS
 *
 * Called by mbedTLS entropy collector when MBEDTLS_ENTROPY_HARDWARE_ALT is
 * defined. Fills the output buffer with pseudo-random bytes derived from
 * system timers and state.
 *
 * Returns 0 on success.
 */
int mbedtls_hardware_poll(void *data,
                          unsigned char *output,
                          size_t len,
                          size_t *olen)
{
    (void)data;

#ifdef __DREAMCAST__
    size_t i;
    uint64_t ns;
    uint32_t sec, msec;
    uint32_t state = 0;

    /* Get high-resolution timer value. */
    ns = timer_ns_gettime64();

    /* Get seconds/milliseconds for additional mixing. */
    timer_ms_gettime(&sec, &msec);

    /* Simple PRNG state seeded from timer. */
    state = (uint32_t)(ns ^ (ns >> 32));
    state ^= (sec * 1000U + msec);

    /* Mix in maple bus frame counter if available for more variation. */
    state ^= maple_state.dma_cntr;

    /*
     * Generate output bytes using a simple xorshift32 PRNG.
     * This is not cryptographically secure but provides reasonable
     * randomness for session keys.
     */
    for (i = 0; i < len; i++) {
        /* xorshift32 step */
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;

        /* Mix in timer again for each byte to add jitter. */
        if ((i & 0x0F) == 0) {
            ns = timer_ns_gettime64();
            state ^= (uint32_t)(ns & 0xFFFFFFFF);
        }

        output[i] = (unsigned char)(state & 0xFF);
    }

    if (olen != NULL) {
        *olen = len;
    }
#else
    /* Non-Dreamcast stub - should not be reached. */
    (void)output;
    (void)len;
    if (olen != NULL) {
        *olen = 0;
    }
#endif

    return 0;
}

/*
 * mbedtls_net_send - network send callback for KOS sockets
 *
 * Called by mbedTLS to send data over the network. The ctx parameter
 * is a pointer to the socket file descriptor (as passed by curl).
 *
 * Returns number of bytes sent, or negative mbedTLS error code.
 */
int mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *((int *)ctx);
    ssize_t ret;

    ret = send(fd, buf, len, 0);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    return (int)ret;
}

/*
 * mbedtls_net_recv - network receive callback for KOS sockets
 *
 * Called by mbedTLS to receive data from the network. The ctx parameter
 * is a pointer to the socket file descriptor (as passed by curl).
 *
 * Returns number of bytes received, or negative mbedTLS error code.
 */
int mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *((int *)ctx);
    ssize_t ret;

    ret = recv(fd, buf, len, 0);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    return (int)ret;
}

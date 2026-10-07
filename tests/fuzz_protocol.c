#define _GNU_SOURCE

#include "kitty_pty_broker.h"
#include "internal.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The harness must never hang, whatever the environment does to it.  kpb_receive
 * waits for the first byte of a frame with no bound (that is the live stream), so
 * the harness guarantees one of two things before calling it: the input has been
 * delivered and the write side closed (the reader then sees data or end of file
 * at once), or it stops here with a message.  Silently waiting would look like a
 * hang in the fuzzer - which is what a sandbox that refuses shutdown() used to
 * produce on the empty input. */
static void
fail_fast(const char *what) {
    fprintf(stderr, "fuzz_protocol: %s (%s)\n", what, strerror(errno));
    abort();
}

static void
finish_input(int writer) {
    if (shutdown(writer, SHUT_WR) != 0) fail_fast("shutdown(SHUT_WR) failed");
}

static void
check_event(kpb_result result, const kpb_event *event, size_t capacity) {
    if (result < KPB_OK || result > KPB_ERR_TIMEOUT) abort();
    if (result != KPB_OK) return;
    switch (event->type) {
        case KPB_EVENT_OUTPUT:
        case KPB_EVENT_RESET:
        case KPB_EVENT_ERROR:
            if (event->size > capacity) abort();
            break;
        case KPB_EVENT_REPLAY_DONE:
        case KPB_EVENT_EXIT:
            if (event->size != 0) abort();
            break;
        default:
            abort();
    }
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    unsigned char storage[KPB_IO_CHUNK + 1U];
    unsigned char *buffer = storage + 1U;
    size_t capacity = KPB_IO_CHUNK;
    int sockets[2];
    kpb_connection connection;
    kpb_event event;
    kpb_result result;

    if (!data || size > 65536U || socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        return 0;
    }
    /* A writer that cannot make progress must fail, not block. */
    if (fcntl(sockets[0], F_SETFL, fcntl(sockets[0], F_GETFL, 0) | O_NONBLOCK) != 0) {
        fail_fast("fcntl(O_NONBLOCK) failed");
    }
    if (size && (data[0] & 4U)) capacity = sizeof(kpb_wire_exit);
    if (size >= 2U && (data[0] & 1U)) {
        static const uint16_t types[] = {
            KPB_FRAME_OUTPUT,
            KPB_FRAME_RESET,
            KPB_FRAME_REPLAY_DONE,
            KPB_FRAME_EXIT,
            KPB_FRAME_ERROR,
            UINT16_MAX
        };
        kpb_frame_header header;
        const void *payload = data + 2U;
        uint32_t payload_size = (uint32_t)(size - 2U);
        uint16_t type = types[data[1] % (sizeof types / sizeof types[0])];
        uint32_t exit_payload = size >= 6U
            ? ((uint32_t)data[2] << 24U) | ((uint32_t)data[3] << 16U) |
              ((uint32_t)data[4] << 8U) | (uint32_t)data[5]
            : 0U;
        if (type == KPB_FRAME_REPLAY_DONE) payload_size = 0;
        if (type == KPB_FRAME_EXIT) {
            payload = &exit_payload;
            payload_size = sizeof exit_payload;
        }
        header.magic = htonl(KPB_PROTOCOL_MAGIC);
        header.version = htons(KPB_PROTOCOL_VERSION);
        header.type = htons(type);
        header.payload_size = htonl(payload_size);
        /* If it cannot all be delivered without blocking, the reader gets what
         * arrived and then end of file. */
        if (write_all_fd(sockets[0], &header, sizeof header) >= 0 && payload_size) {
            (void)write_all_fd(sockets[0], payload, payload_size);
        }
        finish_input(sockets[0]);
    } else {
        (void)(size && write_all_fd(sockets[0], data, size));
        finish_input(sockets[0]);
    }
    {
        struct pollfd ready = {.fd = sockets[1], .events = POLLIN, .revents = 0};
        int polled;
        do {
            polled = poll(&ready, 1, 1000);
        } while (polled < 0 && errno == EINTR);
        if (polled <= 0) fail_fast("reader saw neither data nor end of file");
    }
    memset(&connection, 0, sizeof connection);
    connection.fd = sockets[1];
    memset(&event, 0xa5, sizeof event);
    result = kpb_receive(&connection, buffer, capacity, &event);
    check_event(result, &event, capacity);
    close(sockets[0]);
    close(sockets[1]);
    return 0;
}

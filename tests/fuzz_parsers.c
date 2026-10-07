/* libFuzzer harness for the parsers behind list, reap and the archive - the
 * code that reads bytes a broker, a file or a directory entry supplies and that
 * tests/fuzz_protocol.c (which drives only kpb_receive) does not reach:
 *
 *   parse_metadata       a session's metadata file, which a crashed or hostile
 *                        same-user process may have written
 *   parse_archive_stem   archive file names in reaped/ (".journal" and ".meta")
 *   list_receive         the reply parser behind `list`, fed through a socket
 *                        pair in data-driven chunk sizes
 *
 * The library source is included directly because these functions are static.
 * Every path here is non-blocking, so the harness cannot hang. */
#include "kitty_pty_broker.c"

#include <fcntl.h>

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    {   /* metadata: NUL terminated, as read_small_file leaves its buffer */
        char text[2048];
        metadata_info info;
        size_t length = size < sizeof text - 1 ? size : sizeof text - 1;
        memcpy(text, data, length);
        text[length] = '\0';
        (void)parse_metadata(text, &info);
    }
    {   /* archive names, both kinds */
        char name[256];
        char id[KPB_SESSION_ID_MAX + 1];
        uint64_t started;
        size_t length = size < sizeof name - 1 ? size : sizeof name - 1;
        memcpy(name, data, length);
        name[length] = '\0';
        if (parse_archive_name(name, id, &started) &&
            (strlen(id) > KPB_SESSION_ID_MAX || !valid_component(id))) {
            abort();
        }
        if (parse_archive_stem(name, ".meta", id, &started) &&
            (strlen(id) > KPB_SESSION_ID_MAX || !valid_component(id))) {
            abort();
        }
    }
    {   /* the list reply parser */
        int pair[2];
        list_slot slot;
        size_t offset = 0;
        int turns = 0;
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) return 0;
        if (fcntl(pair[1], F_SETFL, fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK) != 0) abort();
        memset(&slot, 0, sizeof slot);
        slot.fd = pair[0];
        slot.state = LIST_ACTIVE;
        slot.buffer = malloc(LIST_REPLY_SIZE);
        if (!slot.buffer) {
            close(pair[0]);
            close(pair[1]);
            return 0;
        }
        while (offset < size && slot.state != LIST_DONE && turns++ < 64) {
            size_t chunk = 1 + (data[offset] % 97);
            if (chunk > size - offset) chunk = size - offset;
            if (write(pair[1], data + offset, chunk) < 0) break;
            offset += chunk;
            list_receive(&slot);
        }
        if (slot.state != LIST_DONE) list_finish(&slot, KPB_ERR_TIMEOUT);
        free(slot.status);
        close(pair[1]);
    }
    return 0;
}

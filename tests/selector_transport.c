/* Test-only transport probe, included source exposes the actual client helper.
 * Used against owned disposable real receivers, never installed or exported. */
#include "kitty_pty_broker.c"

int
main(int argc, char **argv) {
    kpb_selector_result output;
    struct timespec deadline;
    kpb_result result;
    if (argc != 4) return 2;
    memset(&output, 0, sizeof output);
    deadline_in(&deadline, 150);
    result = selector_terminate(argv[1], argv[2], strtoull(argv[3], NULL, 10), 0, &deadline, &output);
    printf("{\"code\":%d,\"request_sent\":%s,\"receiver_refused\":%s}\n", result,
           output.may_have_sent ? "true" : "false", output.receiver_refused ? "true" : "false");
    return 0;
}

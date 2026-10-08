/* The bounded codecs and descriptor reader, including a grow-after-fstat
 * schedule that cannot be reached deterministically through the public CLI. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <unistd.h>
static ssize_t audited_read(int fd, void *buffer, size_t size);
static int audited_fstat(int fd, struct stat *status);
#define read audited_read
#define fstat audited_fstat
#include "kitty_pty_broker.c"
#undef read
#undef fstat

#include <stdio.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while (0)

static bool auditing;
static int grow_fd = -1;
static size_t requested_max, read_total;

static ssize_t
audited_read(int fd, void *buffer, size_t size) {
    ssize_t result;
    if (auditing && size > requested_max) requested_max = size;
    result = read(fd, buffer, size);
    if (auditing && result > 0) read_total += (size_t)result;
    return result;
}

static int
audited_fstat(int fd, struct stat *status) {
    int result = fstat(fd, status);
    if (auditing && result == 0 && grow_fd >= 0) {
        CHECK(ftruncate(grow_fd, 8192) == 0);
        grow_fd = -1;
    }
    return result;
}

static void
test_codec(void) {
    char *argv[] = {"", "sp ace", "'\"\\", "\n\r\t\b\f\001\037\177", "café 🙂", NULL};
    char *bad[] = {"a\377b\342(", NULL};
    kpb_recorded_command recorded;
    record_start_command(&recorded, argv, "/space ' \"\\\n\001/café");
    CHECK(strcmp(recorded.argv_json,
        "[\"\",\"sp ace\",\"'\\\"\\\\\",\"\\u000a\\u000d\\u0009\\u0008\\u000c\\u0001\\u001f\\u007f\",\"café 🙂\"]") == 0);
    CHECK(strcmp(recorded.cwd_json, "\"/space ' \\\"\\\\\\u000a\\u0001/café\"") == 0);
    CHECK(!recorded.truncated);
    CHECK(valid_recorded_json(recorded.argv_json, true));
    CHECK(valid_recorded_json(recorded.cwd_json, false));
    record_start_command(&recorded, bad, "/bad\377");
    CHECK(strcmp(recorded.argv_json, "[\"a\\ufffdb\\ufffd(\"]") == 0);
    CHECK(strcmp(recorded.cwd_json, "\"/bad\\ufffd\"") == 0);
    CHECK(recorded.truncated);
}

static void
test_bounds(void) {
    char argument[1023], cwd[513], escaped[87], unicode[515], escaped_argument[172];
    char *argv[] = {argument, NULL, NULL};
    kpb_recorded_command recorded;
    memset(argument, 'a', 1020); argument[1020] = '\0';
    memset(cwd, 'c', 510); cwd[510] = '\0';
    record_start_command(&recorded, argv, cwd);
    CHECK(strlen(recorded.argv_json) == 1024);
    CHECK(strlen(recorded.cwd_json) == 512);
    CHECK(!recorded.truncated);
    argv[1] = "tail";
    record_start_command(&recorded, argv, cwd);
    CHECK(strlen(recorded.argv_json) == 1024 && recorded.truncated);
    CHECK(strstr(recorded.argv_json, "tail") == NULL);
    argv[1] = NULL; argument[1020] = 'a'; argument[1021] = '\0';
    record_start_command(&recorded, argv, cwd);
    CHECK(strcmp(recorded.argv_json, "[]") == 0 && recorded.truncated);
    argv[0] = "head"; argv[1] = argument;
    record_start_command(&recorded, argv, cwd);
    CHECK(strcmp(recorded.argv_json, "[\"head\"]") == 0 && recorded.truncated);
    /* A partial escaped string can fit the array even when a partial ASCII
     * string cannot. It must still be omitted as a whole argv element. */
    memset(escaped_argument, '\001', 171); escaped_argument[171] = '\0';
    argv[0] = escaped_argument; argv[1] = NULL;
    record_start_command(&recorded, argv, cwd);
    CHECK(strcmp(recorded.argv_json, "[]") == 0 && recorded.truncated);
    argv[0] = "head";
    argv[1] = NULL; cwd[510] = 'c'; cwd[511] = '\0';
    record_start_command(&recorded, argv, cwd);
    CHECK(strlen(recorded.cwd_json) == 512 && recorded.truncated);
    memset(escaped, '\001', 86); escaped[86] = '\0';
    record_start_command(&recorded, argv, escaped);
    CHECK(strlen(recorded.cwd_json) == 512 && recorded.truncated);
    CHECK(valid_recorded_json(recorded.cwd_json, false));
    for (int index = 0; index < 256; index++) memcpy(unicode + index * 2, "é", 2);
    unicode[512] = '\0';
    record_start_command(&recorded, argv, unicode);
    CHECK(strlen(recorded.cwd_json) == 512 && recorded.truncated);
    CHECK(valid_recorded_json(recorded.cwd_json, false));
}

static void
test_parser(void) {
    kpb_recorded_command recorded;
    const char *invalid[] = {"null", "{}", "[1]", "[\"x\",]", "[\"x\"] ,\"injected\":1",
        "[\"\\q\"]", "[\"\\u12\"]", "[\"\\ud800\"]", "[\"\\udc00\"]", "[\"\377\"]", NULL};
    char data[4097];
    parse_recorded_metadata("broker_pid=1\n", &recorded);
    CHECK(!recorded.argv_json[0] && !recorded.cwd_json[0] && !recorded.have_started && !recorded.truncated);
    parse_recorded_metadata("broker_pid=1\nstarted_millis=0\n", &recorded);
    CHECK(!recorded.argv_json[0] && !recorded.cwd_json[0] && recorded.have_started && recorded.started_millis == 0);
    parse_recorded_metadata("argv_json=[\"x\",\"y\\n\"]\ncwd_json=\"/x\"\nargv_truncated=1\nstarted_millis=42\n", &recorded);
    CHECK(strcmp(recorded.argv_json, "[\"x\",\"y\\n\"]") == 0);
    CHECK(strcmp(recorded.cwd_json, "\"/x\"") == 0);
    CHECK(recorded.truncated && recorded.have_started && recorded.started_millis == 42);
    CHECK(valid_recorded_json("[ \"\\ud83d\\ude42\" , \"a\" ]", true));
    for (int index = 0; invalid[index]; index++) {
        CHECK(snprintf(data, sizeof data, "argv_json=%s\ncwd_json=%s\n", invalid[index], invalid[index]) > 0);
        parse_recorded_metadata(data, &recorded);
        CHECK(!recorded.argv_json[0] && !recorded.cwd_json[0] && recorded.truncated);
    }
    strcpy(data, "argv_json=[\""); memset(data + 12, 'a', 1021); strcpy(data + 1033, "\"]\n");
    parse_recorded_metadata(data, &recorded);
    CHECK(!recorded.argv_json[0] && recorded.truncated);
    strcpy(data, "cwd_json=\""); memset(data + 10, 'c', 511); strcpy(data + 521, "\"\n");
    parse_recorded_metadata(data, &recorded);
    CHECK(!recorded.cwd_json[0] && recorded.truncated);
}

static void
test_read_bound(void) {
    char path[] = "/tmp/kbrec-read.XXXXXX";
    char buffer[8193];
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, 4096) == 0);
    auditing = true;
    CHECK(read_identity_file(path, buffer, sizeof buffer) == 4096);
    CHECK(requested_max <= 4096 && read_total == 4096);
    CHECK(ftruncate(fd, 4097) == 0);
    read_total = requested_max = 0;
    CHECK(read_identity_file(path, buffer, sizeof buffer) < 0);
    CHECK(read_total == 0 && requested_max == 0);
    CHECK(ftruncate(fd, 1) == 0);
    grow_fd = fd;
    CHECK(read_identity_file(path, buffer, sizeof buffer) == 4096);
    CHECK(requested_max <= 4096 && read_total == 4096);
    auditing = false;
    close(fd);
    CHECK(unlink(path) == 0);
}

int
main(int argc, char **argv) {
    if (argc == 1 || strcmp(argv[1], "codec") == 0) test_codec();
    if (argc == 1 || strcmp(argv[1], "bounds") == 0) test_bounds();
    if (argc == 1 || strcmp(argv[1], "parser") == 0) test_parser();
    if (argc == 1 || strcmp(argv[1], "read-bound") == 0) test_read_bound();
    puts("recorded command codec/parser/read-bound tests passed");
    return 0;
}

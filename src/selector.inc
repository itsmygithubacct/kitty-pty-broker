/* Shares the bounded status engine, but never calls list/reap or locks transitions. */
static bool
selector_space(unsigned char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static bool
valid_selector(const kpb_selector *selector) {
    size_t index, bytes = 0;
    if (!selector || selector->attached < -1 || selector->attached > 1) return false;
    if (selector->kind == KPB_SELECTOR_ID || selector->kind == KPB_SELECTOR_PREFIX)
        return selector->reference && (valid_component(selector->reference) ||
            (selector->kind == KPB_SELECTOR_PREFIX &&
             (!strcmp(selector->reference, ".") || !strcmp(selector->reference, "..")))) &&
            !selector->words && !selector->word_count;
    if (selector->kind != KPB_SELECTOR_STARTUP_WORDS || selector->reference || !selector->words ||
        !selector->word_count || selector->word_count > KPB_SELECTOR_MAX_WORDS) return false;
    for (index = 0; index < selector->word_count; index++) {
        const unsigned char *word = (const unsigned char *)selector->words[index];
        if (!word || !*word) return false;
        while (*word) {
            int size;
            if (*word < 0x21 || *word == 0x7f || !(size = utf8_character_size(word))) return false;
            bytes += (size_t)size;
            if (bytes > KPB_SELECTOR_MAX_BYTES) return false;
            word += size;
        }
        if (index + 1 < selector->word_count && ++bytes > KPB_SELECTOR_MAX_BYTES) return false;
    }
    return true;
}

/* Decode validated JSON argv to joined startup text. NUL cannot occur in exec
 * argv and supplies no evidence. The JSON bound also bounds decoded text. */
static bool
selector_startup_text(const char *json, char output[KPB_RECORDED_ARGV_MAX + 1]) {
    const char *cursor = recorded_json_space(json);
    size_t used = 0, arguments = 0;
    if (!valid_recorded_json(json, true) || *cursor++ != '[') return false;
    cursor = recorded_json_space(cursor);
    while (*cursor != ']') {
        if (*cursor++ != '"') return false;
        if (arguments++) {
            if (used == KPB_RECORDED_ARGV_MAX) return false;
            output[used++] = ' ';
        }
        while (*cursor && *cursor != '"') {
            unsigned char encoded[4];
            size_t size = 1;
            encoded[0] = (unsigned char)*cursor++;
            if (encoded[0] == '\\') {
                char escape = *cursor++;
                const char *escapes = "\"\\/bfnrt";
                const char *values = "\"\\/\b\f\n\r\t";
                const char *found = strchr(escapes, escape);
                if (escape == 'u') {
                    unsigned value, low;
                    if (!json_hex4(cursor, &value)) return false;
                    cursor += 4;
                    if (value >= 0xd800 && value <= 0xdbff) {
                        if (cursor[0] != '\\' || cursor[1] != 'u' || !json_hex4(cursor + 2, &low)) return false;
                        cursor += 6;
                        value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
                    }
                    if (!value) return false;
                    if (value < 0x80) encoded[0] = (unsigned char)value;
                    else if (value < 0x800) {
                        size = 2; encoded[0] = 0xc0 | (value >> 6); encoded[1] = 0x80 | (value & 63);
                    } else if (value < 0x10000) {
                        size = 3; encoded[0] = 0xe0 | (value >> 12);
                        encoded[1] = 0x80 | ((value >> 6) & 63); encoded[2] = 0x80 | (value & 63);
                    } else {
                        size = 4; encoded[0] = 0xf0 | (value >> 18); encoded[1] = 0x80 | ((value >> 12) & 63);
                        encoded[2] = 0x80 | ((value >> 6) & 63); encoded[3] = 0x80 | (value & 63);
                    }
                } else {
                    if (!found) return false;
                    encoded[0] = (unsigned char)values[found - escapes];
                }
            }
            if (used + size > KPB_RECORDED_ARGV_MAX) return false;
            memcpy(output + used, encoded, size);
            used += size;
        }
        if (*cursor++ != '"') return false;
        cursor = recorded_json_space(cursor);
        if (*cursor == ',') cursor = recorded_json_space(cursor + 1);
    }
    output[used] = '\0';
    return arguments > 0;
}

static bool
selector_word_match(const kpb_selector *selector, const char *json) {
    char text[KPB_RECORDED_ARGV_MAX + 1];
    char *words[KPB_RECORDED_ARGV_MAX / 2 + 1];
    char *cursor;
    size_t count = 0, index, word;
    if (!selector_startup_text(json, text)) return false;
    cursor = text;
    while (*cursor) {
        while (*cursor && selector_space((unsigned char)*cursor)) cursor++;
        if (!*cursor) break;
        words[count++] = cursor;
        while (*cursor && !selector_space((unsigned char)*cursor)) cursor++;
        if (*cursor) *cursor++ = '\0';
    }
    for (index = 0; index + selector->word_count <= count; index++) {
        for (word = 0; word < selector->word_count; word++)
            if (strcmp(words[index + word], selector->words[word]) != 0) break;
        if (word == selector->word_count) return true;
    }
    return false;
}

static int
selector_open_directory(int parent, const char *name) {
    struct stat info;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &info) != 0 || info.st_uid != geteuid() || (info.st_mode & 077)) {
        close(fd); return -1;
    }
    return fd;
}

/* Old display parsing is unchanged. Selector evidence requires a complete
 * regular file in a pinned private directory and unique critical fields.
 * Missing, duplicated, truncated or inconsistent facts are UNKNOWN. */
static bool
selector_recorded(int directory, const char *id, const kpb_status *live,
                  kpb_recorded_command *recorded) {
    char data[KPB_IDENTITY_FILE_MAX + 2];
    struct stat info;
    size_t used = 0;
    unsigned ids = 0, starts = 0, argvs = 0, truncated = 0, pids = 0;
    bool correct_id = false, complete = false;
    metadata_info identity;
    const char *line;
    int fd;
    if (directory < 0) return false;
    fd = openat(directory, "metadata", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false; /* provisional records have no complete startup facts */
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 077) || info.st_size <= 0 || info.st_size > KPB_IDENTITY_FILE_MAX) {
        close(fd); return false;
    }
    while (used < sizeof data - 1) {
        ssize_t got = read(fd, data + used, sizeof data - 1 - used);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { close(fd); return false; }
        if (!got) break;
        used += (size_t)got;
    }
    close(fd);
    if (!used || used > KPB_IDENTITY_FILE_MAX || used != (size_t)info.st_size ||
        data[used - 1] != '\n' || memchr(data, '\0', used)) return false;
    data[used] = '\0';
    for (line = data; line && *line;) {
        size_t length = strcspn(line, "\n");
        if (strncmp(line, "id=", 3) == 0) {
            ids++; correct_id = length == strlen(id) + 3 && memcmp(line + 3, id, length - 3) == 0;
        } else if (strncmp(line, "started_millis=", 15) == 0) starts++;
        else if (strncmp(line, "broker_pid=", 11) == 0) pids++;
        else if (strncmp(line, "argv_json=", 10) == 0) argvs++;
        else if (strncmp(line, "argv_truncated=", 15) == 0) {
            truncated++; complete = length == 16 && line[15] == '0';
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    if (ids != 1 || !correct_id || starts != 1 || pids != 1 || argvs != 1 || truncated != 1 || !complete ||
        !parse_metadata(data, &identity) || !identity.have_started || !identity.started_millis) return false;
    parse_recorded_metadata(data, recorded);
    if (!recorded->argv_json[0] || recorded->truncated || !recorded->have_started) return false;
    if (live && (strcmp(live->session_id, id) || live->started_millis != identity.started_millis ||
                 live->broker_pid != identity.pid)) return false;
    { char text[KPB_RECORDED_ARGV_MAX + 1];
      return selector_startup_text(recorded->argv_json, text); }
}

/* 0 proved nonmatch, 1 proved match, 2 could match. Conjunction precedes count:
 * a known attachment nonmatch excludes even unknown startup evidence. */
static int
selector_match(const kpb_selector *selector, const char *id, const kpb_status *live, int directory) {
    int discriminator = 1, attachment = 1;
    if (selector->kind == KPB_SELECTOR_ID) discriminator = strcmp(id, selector->reference) == 0;
    else if (selector->kind == KPB_SELECTOR_PREFIX)
        discriminator = strncmp(id, selector->reference, strlen(selector->reference)) == 0;
    else {
        kpb_recorded_command recorded;
        discriminator = selector_recorded(directory, id, live, &recorded)
            ? (selector_word_match(selector, recorded.argv_json) ? 1 : 0) : 2;
    }
    if (selector->attached != -1)
        attachment = live ? (live->attached == selector->attached ? 1 : 0) : 2;
    if (!discriminator || !attachment) return 0;
    return discriminator == 2 || attachment == 2 ? 2 : 1;
}

static kpb_result
selector_resolve_until(const char *runtime_dir, const kpb_selector *selector,
                       const struct timespec *deadline, kpb_selector_result *output) {
    list_slot *slots = NULL;
    int *directories = NULL;
    int runtime = -1, sessions = -1;
    DIR *walk = NULL;
    size_t count = 0, index;
    kpb_result result = KPB_OK;
    if (!output) return KPB_ERR_INVALID;
    memset(output, 0, sizeof *output);
    if (!valid_selector(selector) || !runtime_dir || runtime_dir[0] != '/') return KPB_ERR_INVALID;
    runtime = selector_open_directory(AT_FDCWD, runtime_dir);
    if (runtime < 0) return KPB_ERR_SECURITY;
    sessions = selector_open_directory(runtime, "sessions");
    if (sessions < 0) {
        result = errno == ENOENT ? KPB_OK : KPB_ERR_SECURITY;
        if (result == KPB_OK && millis_until(deadline) <= 0) result = KPB_ERR_TIMEOUT;
        output->complete = result == KPB_OK;
        close(runtime); return result;
    }
    slots = calloc(KPB_SELECTOR_CENSUS_MAX, sizeof *slots);
    directories = malloc(KPB_SELECTOR_CENSUS_MAX * sizeof *directories);
    if (!slots || !directories) { result = KPB_ERR_SYSTEM; goto out; }
    walk = fdopendir(dup(sessions));
    if (!walk) { result = KPB_ERR_SYSTEM; goto out; }
    for (;;) {
        struct dirent *entry;
        errno = 0;
        entry = readdir(walk);
        if (!entry) { if (errno) result = KPB_ERR_SYSTEM; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!valid_component(entry->d_name)) { result = KPB_ERR_SECURITY; break; }
        if (count == KPB_SELECTOR_CENSUS_MAX) { result = KPB_ERR_BUFFER; break; }
        if (millis_until(deadline) <= 0) { result = KPB_ERR_TIMEOUT; break; }
        copy_string(slots[count].id, sizeof slots[count].id, entry->d_name);
        slots[count].fd = -1;
        directories[count] = selector_open_directory(sessions, entry->d_name);
        count++;
    }
    closedir(walk); walk = NULL;
    output->census_count = count;
    if (result != KPB_OK) goto out;
    if (millis_until(deadline) <= 0) { result = KPB_ERR_TIMEOUT; goto out; }
    list_query_all(runtime_dir, slots, count, deadline);
    /* A prompt per-row timeout can prove a nonmatch; exhaustion of the shared
     * census budget cannot certify a complete observation. This also covers
     * decoding the final status after the query engine's last clock check. */
    if (millis_until(deadline) <= 0) { result = KPB_ERR_TIMEOUT; goto out; }
    for (index = 0; index < count; index++) {
        list_slot *slot = &slots[index];
        struct stat pinned, current;
        kpb_status *live = slot->error == KPB_OK ? slot->status : NULL;
        bool stable;
        int match;
        if (millis_until(deadline) <= 0) { result = KPB_ERR_TIMEOUT; goto out; }
        stable = directories[index] >= 0 && fstat(directories[index], &pinned) == 0 &&
            fstatat(sessions, slot->id, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
            pinned.st_dev == current.st_dev && pinned.st_ino == current.st_ino &&
            pinned.st_uid == geteuid() && !(pinned.st_mode & 077);
        if (!stable || (live && strcmp(live->session_id, slot->id))) {
            live = NULL;
            if (directories[index] >= 0) close(directories[index]);
            directories[index] = -1;
        }
        match = selector_match(selector, slot->id, live, directories[index]);
        /* Metadata and pinned-directory checks consume the same budget, even
         * for a row whose predicates ultimately prove it cannot match. */
        if (millis_until(deadline) <= 0) { result = KPB_ERR_TIMEOUT; goto out; }
        if (!match) continue;
        if (match == 2) output->possible++;
        else {
            output->matches++;
            if (live) output->candidate = *live;
        }
        if (output->row_count < KPB_SELECTOR_ROWS_MAX) {
            kpb_selector_row *row = &output->rows[output->row_count++];
            copy_string(row->session_id, sizeof row->session_id, slot->id);
            row->reachable = live != NULL; row->possible = match == 2;
        }
    }
    if (millis_until(deadline) <= 0) { result = KPB_ERR_TIMEOUT; goto out; }
    output->complete = 1;
    output->actionable = output->matches == 1 && output->possible == 0 && output->candidate.started_millis > 0;
out:
    if (walk) closedir(walk);
    for (index = 0; index < count; index++) {
        if (directories && directories[index] >= 0) close(directories[index]);
        if (slots) { if (slots[index].fd >= 0) close(slots[index].fd); free(slots[index].status); free(slots[index].buffer); }
    }
    free(directories); free(slots);
    close(sessions); close(runtime);
    return result;
}

kpb_result
kpb_resolve_selector(const char *runtime_dir, const kpb_selector *selector, int timeout_millis,
                     kpb_selector_result *output) {
    struct timespec deadline;
    deadline_from_timeout(&deadline, timeout_millis, KPB_DEFAULT_LIST_TIMEOUT_MILLIS);
    return selector_resolve_until(runtime_dir, selector, &deadline, output);
}

static kpb_result
selector_terminate(const char *runtime_dir, const char *id, uint64_t started, int attached,
                   const struct timespec *deadline, kpb_selector_result *output) {
    unsigned char payload[128];
    kpb_wire_terminate_condition request;
    uint16_t type = 0;
    uint32_t size = 0;
    int fd;
    bool may_have_sent = false;
    kpb_result result = connect_session_until(runtime_dir, id, &fd, deadline);
    if (result == KPB_ERR_TIMEOUT) return KPB_ERR_NOT_SENT;
    if (result != KPB_OK) return result;
    memset(&request, 0, sizeof request);
    request.expected_started_millis = host_to_be64(started);
    request.expected_attached = htonl((uint32_t)attached);
    result = send_frame_until(fd, KPB_FRAME_TERMINATE, &request,
                        attached == -1 ? sizeof(kpb_wire_terminate) : sizeof request, deadline, &may_have_sent);
    output->may_have_sent = may_have_sent;
    if (result == KPB_ERR_TIMEOUT && !may_have_sent) result = KPB_ERR_NOT_SENT;
    if (result == KPB_OK) result = receive_frame_bounded(fd, &type, payload, sizeof payload, &size, deadline);
    close(fd);
    if (result != KPB_OK) return result;
    if (type == KPB_FRAME_ACK && size == 0) return KPB_OK;
    if (type == KPB_FRAME_ERROR) {
        if (size == strlen(KPB_ERROR_IDENTITY_MISMATCH) && !memcmp(payload, KPB_ERROR_IDENTITY_MISMATCH, size)) {
            output->receiver_refused = 1; return KPB_ERR_MISMATCH;
        }
        if (size == strlen(KPB_ERROR_ATTACHMENT_MISMATCH) && !memcmp(payload, KPB_ERROR_ATTACHMENT_MISMATCH, size)) {
            output->receiver_refused = 1; return KPB_ERR_ATTACHMENT;
        }
        if (size == strlen(KPB_ERROR_INVALID) && !memcmp(payload, KPB_ERROR_INVALID, size)) {
            output->receiver_refused = 1; return KPB_ERR_UNSUPPORTED;
        }
    }
    return KPB_ERR_PROTOCOL;
}

kpb_result
kpb_end_selector(const char *runtime_dir, const kpb_selector *selector, const char *resolved_id,
                 uint64_t expected_started_millis, int timeout_millis, kpb_selector_result *output) {
    const char *caller = getenv("KITTY_PTY_BROKER_SESSION");
    struct timespec deadline;
    kpb_status now;
    int runtime, sessions, directory, match;
    kpb_result result;
    if (!output) return KPB_ERR_INVALID;
    memset(output, 0, sizeof *output);
    if (!valid_selector(selector) || !valid_component(resolved_id) || !expected_started_millis) return KPB_ERR_INVALID;
    if (!valid_component(caller)) return KPB_ERR_CALLER;
    deadline_from_timeout(&deadline, timeout_millis, KPB_DEFAULT_TIMEOUT_MILLIS);
    result = selector_resolve_until(runtime_dir, selector, &deadline, output);
    if (result != KPB_OK) return result;
    if (output->matches + output->possible > 1) return KPB_ERR_AMBIGUOUS;
    if (!output->actionable) return KPB_ERR_UNRESOLVED;
    if (!strcmp(output->candidate.session_id, caller)) return KPB_ERR_CALLER;
    if (strcmp(output->candidate.session_id, resolved_id) || output->candidate.started_millis != expected_started_millis)
        return KPB_ERR_MISMATCH;
    if (millis_until(&deadline) <= 0) return KPB_ERR_TIMEOUT;
    result = query_status_until(runtime_dir, resolved_id, &now, &deadline);
    if (result != KPB_OK) return result;
    if (strcmp(now.session_id, resolved_id) || now.started_millis != expected_started_millis) return KPB_ERR_MISMATCH;
    runtime = selector_open_directory(AT_FDCWD, runtime_dir);
    sessions = runtime < 0 ? -1 : selector_open_directory(runtime, "sessions");
    directory = sessions < 0 ? -1 : selector_open_directory(sessions, resolved_id);
    match = selector_match(selector, resolved_id, &now, directory);
    if (directory >= 0) close(directory);
    if (sessions >= 0) close(sessions);
    if (runtime >= 0) close(runtime);
    if (directory < 0 || match != 1) return KPB_ERR_UNRESOLVED;
    if (millis_until(&deadline) <= 0) return KPB_ERR_TIMEOUT;
    return selector_terminate(runtime_dir, resolved_id, expected_started_millis, selector->attached, &deadline, output);
}

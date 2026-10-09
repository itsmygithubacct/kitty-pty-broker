"""Extract request transport bodies into the effect-denied memory C fixture.

No shipping library, receiver, backend, process or native socket is linked.
The old-source bridge is explicitly fixture-only and never shipped.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

from selector_inert import function


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--no-dontwait', action='store_true')
    ap.add_argument('--mutant', choices=('nonblocking', 'feature-detection', 'reset-budget', 'iov', 'send-knowledge',
                                       'connect-expiry', 'list-expiry', 'receive-expiry',
                                       'precision', 'selector-budget', 'post-ready',
                                       'equivalent'))
    args = ap.parse_args()
    args.output.mkdir(mode=0o700, parents=True)
    files = {n: (args.source / n).read_bytes() for n in
             ('src/kitty_pty_broker.c', 'src/selector.inc', 'src/protocol.h', 'include/kitty_pty_broker.h')}
    core, selector = files['src/kitty_pty_broker.c'].decode(), files['src/selector.inc'].decode()
    old = 'send_frame_until(' not in core
    if old:
        features = '''/* Fixture-only feature tag; the old product had none. */
#ifdef MSG_DONTWAIT
#define KPB_HAVE_MSG_DONTWAIT 1
#else
#define MSG_DONTWAIT 0
#define KPB_HAVE_MSG_DONTWAIT 0
#endif
'''
    else:
        start = core.index('#ifdef MSG_DONTWAIT\n')
        features = core[start:core.index('#endif', start)+len('#endif')]
    original_features = features
    blocks = {}
    names = ['host_to_be64', 'be64_to_host', 'copy_string', 'valid_component', 'deadline_in', 'millis_until', 'deadline_from_timeout',
             'read_all_bounded', 'receive_frame_bounded', 'connect_session_until',
             'kpb_query_status_timeout', 'kpb_terminate_timeout', 'kpb_terminate_expect',
             'kpb_attach_timeout', 'kpb_attach_with_options_timeout', 'kpb_send_input', 'kpb_resize', 'kpb_detach',
             'list_finish', 'list_start', 'list_receive', 'list_query_all']
    if old:
        names += ['send_frame']
    else:
        names += ['sendmsg_nonblocking', 'send_frame_until', 'send_frame', 'attach_export',
                  'query_status_until', 'list_send']
    for name in names:
        blocks[name] = function(core, name)
    for name in ['selector_terminate', 'kpb_end_selector']:
        blocks[name] = function(selector, name)
    originals = dict(blocks)
    def replace(name, old_text, new_text):
        assert blocks[name].count(old_text) == 1, (name, old_text)
        blocks[name] = blocks[name].replace(old_text, new_text, 1)
    if args.mutant:
        assert not old
        m = args.mutant
        if m == 'nonblocking':
            replace('sendmsg_nonblocking', 'MSG_NOSIGNAL | MSG_DONTWAIT', 'MSG_NOSIGNAL')
        elif m == 'feature-detection':
            assert features.count('#define KPB_HAVE_MSG_DONTWAIT 1') == 1
            features = features.replace('#define KPB_HAVE_MSG_DONTWAIT 1',
                                        '#define KPB_HAVE_MSG_DONTWAIT 0', 1)
        elif m == 'reset-budget':
            replace('send_frame_until', 'size_t vector = 0;', 'size_t vector = 0; struct timespec renewed;')
            replace('send_frame_until', 'long remaining = millis_until(deadline);',
                    'deadline_in(&renewed, 5); deadline = &renewed; long remaining = millis_until(deadline);')
        elif m == 'iov':
            replace('send_frame_until', '(unsigned char *)vectors[vector].iov_base + consumed;',
                    '(unsigned char *)vectors[vector].iov_base;')
        elif m == 'send-knowledge':
            replace('send_frame_until', 'if (may_have_sent) *may_have_sent = true;',
                    'if (may_have_sent) *may_have_sent = false;')
        elif m == 'connect-expiry':
            replace('connect_session_until',
                    'if (millis_until(deadline) <= 0) { close(fd); return KPB_ERR_TIMEOUT; }\n        if (connect',
                    '(void)deadline;\n        if (connect')
        elif m == 'list-expiry':
            replace('list_send', 'if (millis_until(deadline) <= 0) { list_finish(slot, KPB_ERR_TIMEOUT); return; }\n        memset',
                    '(void)deadline;\n        memset')
        elif m == 'receive-expiry':
            replace('read_all_bounded', 'size_t received = 0;', 'size_t received = 0; struct timespec renewed;')
            replace('read_all_bounded', 'long remaining = millis_until(deadline);',
                    'deadline_in(&renewed, 5); deadline = &renewed; long remaining = millis_until(deadline);')
        elif m == 'precision':
            replace('millis_until', 'deadline->tv_nsec <= moment.tv_nsec', 'deadline->tv_nsec < moment.tv_nsec')
            replace('millis_until', '(uintmax_t)(nanos + 999999L) / 1000000U', '(uintmax_t)nanos / 1000000U + 1U')
        elif m == 'selector-budget':
            replace('kpb_end_selector', 'result = query_status_until(runtime_dir, resolved_id, &now, &deadline);',
                    'result = kpb_query_status_timeout(runtime_dir, resolved_id, &now, timeout_millis);')
        elif m == 'post-ready':
            replace('send_frame_until', 'long remaining = millis_until(deadline);',
                    'long remaining = vector ? millis_until(deadline) : 5;')
        elif m == 'equivalent':
            replace('sendmsg_nonblocking', 'MSG_NOSIGNAL | MSG_DONTWAIT', 'MSG_DONTWAIT | MSG_NOSIGNAL')
    slots = core[core.index('typedef enum { LIST_PENDING'):core.index('#define LIST_REPLY_SIZE')]
    paths = core[core.index('typedef struct {\n    char session_id[KPB_SESSION_ID_MAX + 1];'):core.index('/* A bounded queue')]
    fixture = Path(__file__).with_name('send_deadline_inert.c.in').read_text()
    values = {
        'FEATURES': features,
        'TYPES': paths + '\n' + slots + '\n#define KPB_LIST_WINDOW 128\n#define LIST_REPLY_SIZE (sizeof(kpb_frame_header) + sizeof(kpb_wire_status))',
        'BASE': '\n\n'.join(blocks[n] for n in ['host_to_be64', 'be64_to_host', 'copy_string', 'valid_component', 'deadline_in', 'millis_until', 'deadline_from_timeout']),
        'SEND': ('\n\n'.join(blocks[n] for n in ['send_frame']) + '''
static kpb_result send_frame_until(int fd, uint16_t type, const void *payload, uint32_t size,
                                 const struct timespec *deadline, bool *sent) {
    size_t before = memory_fds[fd].written;
    kpb_result r; (void)deadline;
    r = send_frame(fd, type, payload, size);
    if (sent) *sent = memory_fds[fd].written != before;
    return r;
}
''' if old else '\n\n'.join(blocks[n] for n in ['sendmsg_nonblocking', 'send_frame_until', 'send_frame'])),
        'READ': '\n\n'.join(blocks[n] for n in ['read_all_bounded', 'receive_frame_bounded']),
        'CONNECT': blocks['connect_session_until'],
        'STATUS': '\n\n'.join(blocks[n] for n in (['kpb_query_status_timeout'] if old else ['query_status_until', 'kpb_query_status_timeout'])),
        'APIS': '\n\n'.join(blocks[n] for n in ['kpb_terminate_timeout', 'kpb_terminate_expect'] +
                            ([] if old else ['attach_export']) + ['kpb_attach_timeout', 'kpb_attach_with_options_timeout',
                                                                  'kpb_send_input', 'kpb_resize', 'kpb_detach']),
        'LIST': '\n\n'.join(blocks[n] for n in ['list_finish', 'list_start'] + ([] if old else ['list_send']) +
                            ['list_receive', 'list_query_all']),
        'SELECTOR': '\n\n'.join(blocks[n] for n in ['selector_terminate', 'kpb_end_selector']),
        'OLD': '1' if old else '0',
    }
    for key, value in values.items():
        assert fixture.count('@' + key + '@') == 1, key
        fixture = fixture.replace('@' + key + '@', value)
    if args.no_dontwait:
        fixture = '#define TEST_NO_DONTWAIT 1\n' + fixture
    for name in ('include/kitty_pty_broker.h', 'src/protocol.h'):
        (args.output / Path(name).name).write_bytes(files[name])
    (args.output / 'fixture.c').write_text(fixture)
    (args.output / 'selected-functions.json').write_text(json.dumps({
        'old_source_bridge': old, 'mutant': args.mutant, 'no_dontwait': args.no_dontwait,
        'feature_guard_extracted_from_product': not old,
        'original_feature_guard_sha256': hashlib.sha256(original_features.encode()).hexdigest(),
        'compiled_feature_guard_sha256': hashlib.sha256(features.encode()).hexdigest(),
        'source_sha256': {k: hashlib.sha256(v).hexdigest() for k,v in files.items()},
        'actual_selected_functions': list(blocks),
        'original_selected_sha256': {k: hashlib.sha256(v.encode()).hexdigest() for k,v in originals.items()},
        'compiled_selected_sha256': {k: hashlib.sha256(v.encode()).hexdigest() for k,v in blocks.items()},
        'adapters': ['memory send/poll/clock/descriptors/connect/read/peer/paths',
                     'memory wire status decoder', 'memory selector census/metadata; no receiver or termination effects'],
        'native_broker_linked': False, 'fixture_sha256': hashlib.sha256(fixture.encode()).hexdigest(),
    },indent=2)+'\n')
    print(json.dumps({'generated':str(args.output),'old_source_bridge':old,'mutant':args.mutant,'functions':len(blocks)}))


if __name__ == '__main__':
    main()

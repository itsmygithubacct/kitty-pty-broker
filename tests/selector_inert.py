"""Extract only actual pure selector/status functions into a memory-only C fixture.
No broker library, native backend, process, socket, selector-end or dispatch linkage.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def function(source, name):
    match = re.search(r'(?m)^' + re.escape(name) + r'\([^;]*?\)\s*\{', source)
    if match is None:
        raise ValueError('missing selected definition: ' + name)
    start = source.rfind('\n', 0, match.start() - 1) + 1
    cursor = match.end() - 1
    depth = 0
    quote = None
    while cursor < len(source):
        char = source[cursor]
        if quote:
            if char == '\\':
                cursor += 2
                continue
            if char == quote:
                quote = None
        elif source.startswith('/*', cursor):
            cursor = source.index('*/', cursor + 2) + 2
            continue
        elif source.startswith('//', cursor):
            cursor = source.index('\n', cursor + 2) + 1
            continue
        elif char in ('"', "'"):
            quote = char
        elif char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0:
                return source[start:cursor + 1]
        cursor += 1
    raise ValueError('unterminated selected definition: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mutant', choices=('separator', 'deadline'))
    parser.add_argument('--baseline-source', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    args.output.mkdir(mode=0o700, parents=True)
    files = {name: (args.source / name).read_bytes() for name in
             ('src/kitty_pty_broker.c', 'src/selector.inc', 'src/protocol.h', 'include/kitty_pty_broker.h')}
    core, selector = (files[name].decode() for name in ('src/kitty_pty_broker.c', 'src/selector.inc'))
    blocks = {}
    for name in ('copy_string', 'valid_component', 'utf8_character_size', 'deadline_in', 'millis_until',
                 'deadline_from_timeout', 'list_finish', 'list_query_all'):
        blocks[name] = function(core, name)
    for name in ('valid_selector', 'selector_space', 'selector_word_match', 'selector_match',
                 'selector_open_directory', 'selector_resolve_until', 'kpb_resolve_selector'):
        blocks[name] = function(selector, name)
    original_blocks = dict(blocks)
    if args.mutant == 'separator':
        before = 'index + 1 < selector->word_count && ++bytes'
        assert blocks['valid_selector'].count(before) == 1
        blocks['valid_selector'] = blocks['valid_selector'].replace(before, '++bytes', 1)
    if args.mutant == 'deadline':
        assert args.baseline_source is not None
        old = (args.baseline_source / 'src/selector.inc').read_text()
        previous = function(old, 'kpb_resolve_selector')
        body = previous[previous.index('{') + 1:]
        assert body.count('    struct timespec deadline;\n') == 1
        body = body.replace('    struct timespec deadline;\n', '', 1)
        setup = '    deadline_from_timeout(&deadline, timeout_millis, KPB_DEFAULT_LIST_TIMEOUT_MILLIS);\n'
        assert body.count(setup) == 1
        body = body.replace(setup, '', 1).replace('&deadline', 'deadline')
        current = blocks['selector_resolve_until']
        blocks['selector_resolve_until'] = current[:current.index('{') + 1] + body
        assert original_blocks['selector_resolve_until'] != blocks['selector_resolve_until']
    slots = core[core.index('typedef enum { LIST_PENDING'):core.index('#define LIST_REPLY_SIZE')]
    window = re.search(r'(?m)^#define KPB_LIST_WINDOW \d+$', core).group()
    replacements = {
        'BASE': '\n\n'.join(blocks[n] for n in ('copy_string', 'valid_component', 'utf8_character_size',
                                               'deadline_in', 'millis_until', 'deadline_from_timeout')) + '\n' + window + '\n' + slots,
        'FINISH': blocks['list_finish'], 'QUERY': blocks['list_query_all'],
        'SELECTOR': '\n\n'.join(blocks[n] for n in ('valid_selector', 'selector_space', 'selector_word_match',
                                                   'selector_match', 'selector_open_directory', 'selector_resolve_until', 'kpb_resolve_selector'))}
    fixture = Path(__file__).with_name('selector_inert.c.in').read_text()
    for key, value in replacements.items():
        assert fixture.count('@' + key + '@') == 1
        fixture = fixture.replace('@' + key + '@', value)
    assert 'selector_terminate(' not in fixture and 'kpb_end_selector(' not in fixture
    (args.output / 'fixture.c').write_text(fixture)
    (args.output / 'kitty_pty_broker.h').write_bytes(files['include/kitty_pty_broker.h'])
    (args.output / 'protocol.h').write_bytes(files['src/protocol.h'])
    (args.output / 'selected-functions.json').write_text(json.dumps({
        'schema': 'selector-inert-selected-c/v1', 'mutant': args.mutant,
        'source_sha256': {k: hashlib.sha256(v).hexdigest() for k, v in files.items()},
        'original_selected_sha256': {k: hashlib.sha256(v.encode()).hexdigest() for k, v in original_blocks.items()},
        'compiled_selected_sha256': {k: hashlib.sha256(v.encode()).hexdigest() for k, v in blocks.items()},
        'fixture_sha256': hashlib.sha256(fixture.encode()).hexdigest(),
        'actual_selected_functions': list(blocks),
        'adapters': ['memory directory enumeration, stat and metadata', 'monotonic memory clock',
                     'in-memory status startup/receive', 'memory poll', 'validated startup text'],
        'native_broker_linked': False, 'dispatch_linked': False,
        'unsent_witness': 'eligibility counter only; kpb_end_selector is never compiled or executed'}, indent=2) + '\n')
    print(json.dumps({'generated': str(args.output), 'selected_functions': len(blocks), 'mutant': args.mutant}))


if __name__ == '__main__':
    main()

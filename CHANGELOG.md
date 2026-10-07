# Changelog

## 0.4.0

Additive library API; the frame-format version stays 1 and no existing wire struct changed.

### Behaviour changes to existing calls

- **`kpb_terminate_timeout` (and the CLI's `kill`) now return `KPB_ERR_NOT_SENT` (14) - and exit 6 -
  where they used to return `KPB_ERR_TIMEOUT` when the deadline expired while still CONNECTING.**
  Nothing had been sent, so nothing was or will be done. `KPB_ERR_TIMEOUT` keeps its old meaning for
  a request that WAS sent and went unanswered ("the broker may still act on it"). A caller that
  retried a kill only on `KPB_ERR_TIMEOUT` should also retry on `KPB_ERR_NOT_SENT`.
- Every client wait is bounded (2 s per operation, 1 s overall for `list`; `--timeout SECONDS`).
  `kpb_list` returns `KPB_ERR_NOT_FOUND` for a missing runtime and `KPB_ERR_SECURITY` for an unsafe one.
- A v1 `kpb_attach` waits (peeking) for a complete first frame; `kpb_receive` bounds the rest of any
  frame once it has begun and may return `KPB_ERR_TIMEOUT`.
- `kpb_spawn` may return `KPB_ERR_TIMEOUT` (sessions lock busy, or the new broker did not answer in
  time - see `kpb_spawn_timeout`).
- Status JSON gains `cwd_now`, `cwd_now_deleted`, `boot_id`, `start_ticks`; existing fields unchanged.

### New

`kpb_terminate_expect` (identity-bound terminate; `KPB_ERR_MISMATCH`, `KPB_ERR_UNSUPPORTED`),
`kpb_list_with_options`, `kpb_*_timeout`, `kpb_spawn_timeout`, `kpb_check_runtime`,
`kpb_session_socket_path`, `kpb_read_cwd_now[_ex]`, `kpb_read_boot_id`, `kpb_read_start_ticks`,
`kpb_list_reaped`, `kpb_reaped_path`; CLI `list --all`, `reaped`, `kill --expect-started`.
Reaped sessions' journals are archived to `RUNTIME/reaped/` (bounded).

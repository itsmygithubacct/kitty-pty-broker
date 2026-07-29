#ifndef KITTY_PTY_BROKER_PROTOCOL_H
#define KITTY_PTY_BROKER_PROTOCOL_H

#include <stdint.h>

/* KPB_PROTOCOL_VERSION is the FRAME-FORMAT version.  It must never be bumped.
 *
 * send_frame() stamps it on every header and receive_frame() rejects any other
 * value, on both sides, so raising it would break every deployed peer in both
 * directions at once - including a plain `status` query.  That matters here
 * because a broker process outlives the frontend by design: an update replaces
 * the client binary while brokers from the previous build keep running.
 *
 * The SESSION protocol version is negotiated inside the attach payload
 * instead, and is discriminated structurally: a v1 attach carries an 8-byte
 * kpb_wire_winsize, a v2 attach carries a 32-byte kpb_wire_attach.  The server
 * emits KPB_FRAME_ATTACH_REPLY if and only if the request was 32 bytes, so a
 * v1 peer can never receive a frame type it does not parse.
 *
 * The negotiated ceiling itself is KPB_PROTOCOL_VERSION_MAX, which lives in
 * the installed header because callers need it to fill kpb_attach_options. */
#define KPB_PROTOCOL_VERSION 1U
#define KPB_PROTOCOL_MAGIC 0x4b504231U
#define KPB_PROTOCOL_MAX_PAYLOAD (1024U * 1024U)

/* Frame types are appended and never renumbered: they are on the wire even
 * though this header is internal and never installed. */
enum kpb_frame_type {
    KPB_FRAME_ATTACH = 1,
    KPB_FRAME_INPUT = 2,
    KPB_FRAME_RESIZE = 3,
    KPB_FRAME_DETACH = 4,
    KPB_FRAME_STATUS = 5,
    KPB_FRAME_TERMINATE = 6,
    KPB_FRAME_OUTPUT = 7,
    KPB_FRAME_REPLAY_DONE = 8,
    KPB_FRAME_EXIT = 9,
    KPB_FRAME_ERROR = 10,
    KPB_FRAME_STATUS_REPLY = 11,
    KPB_FRAME_ACK = 12,
    KPB_FRAME_OBSERVE = 13,
    KPB_FRAME_ATTACH_REPLY = 14
};

/* Attach modes, carried in kpb_wire_attach.mode. */
#define KPB_WIRE_MODE_CONTROL 0U
#define KPB_WIRE_MODE_OBSERVE 1U

/* kpb_wire_attach.flags */
#define KPB_ATTACH_FLAG_RESUME 0x1U

/* kpb_wire_attach_reply.flags */
#define KPB_REPLY_FLAG_RESUMED 0x1U
#define KPB_REPLY_FLAG_COMPLETE 0x2U
#define KPB_REPLY_FLAG_TRUNCATED 0x4U

/* Error payloads.  Always sent through send_error(), which derives the length
 * with strlen: a hand-typed length that disagrees is an immediate over-read. */
#define KPB_ERROR_UNAUTHORIZED "unauthorized peer"
#define KPB_ERROR_INVALID "invalid request"
#define KPB_ERROR_ATTACHED "session already attached"
#define KPB_ERROR_INPUT_LIMIT "pane input buffer exceeded"
#define KPB_ERROR_READ_ONLY "observer connections are read-only"
#define KPB_ERROR_OBSERVER_FULL "observer capacity reached"
#define KPB_ERROR_NEEDS_V2 "observe requires protocol 2"

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payload_size;
} kpb_frame_header;

typedef struct {
    uint16_t rows;
    uint16_t columns;
    uint16_t xpixel;
    uint16_t ypixel;
} kpb_wire_winsize;

/* A v2 attach or observe request.  Bytes 0..7 are byte-identical to
 * kpb_wire_winsize, so the two layouts share a prefix and differ only in
 * length.  Field order is chosen so the struct has no interior padding on any
 * conforming target; the static assertion below is what enforces that, since
 * the server compares payload_size against sizeof exactly. */
typedef struct {
    uint16_t rows;           /*  0 */
    uint16_t columns;        /*  2 */
    uint16_t xpixel;         /*  4 */
    uint16_t ypixel;         /*  6 */
    uint16_t version;        /*  8  client's maximum session version */
    uint16_t mode;           /* 10  KPB_WIRE_MODE_*                  */
    uint32_t flags;          /* 12  KPB_ATTACH_FLAG_*                */
    uint64_t resume_epoch;   /* 16 */
    uint64_t resume_offset;  /* 24 */
} kpb_wire_attach;

/* The server's answer to a v2 request.  journal_offset is the stream position
 * of the FIRST byte of the OUTPUT frames that follow, so a client can adopt it
 * as a cursor and advance it by each OUTPUT payload size. */
typedef struct {
    uint64_t journal_epoch;   /*  0 */
    uint64_t journal_offset;  /*  8 */
    uint32_t flags;           /* 16  KPB_REPLY_FLAG_*        */
    uint16_t result;          /* 20  kpb_result; 0 == accepted */
    uint16_t version;         /* 22  selected session version  */
} kpb_wire_attach_reply;

_Static_assert(sizeof(kpb_frame_header) == 12, "frame header must be 12 bytes");
_Static_assert(sizeof(kpb_wire_attach) == 32, "v2 attach must be 32 bytes");
_Static_assert(
    sizeof(kpb_wire_attach_reply) == 24, "attach reply must be 24 bytes");

typedef struct {
    int32_t wait_status;
} kpb_wire_exit;

typedef struct {
    uint32_t version;
    int64_t broker_pid;
    int64_t child_pid;
    int64_t foreground_pgrp;
    uint64_t started_millis;
    uint64_t journal_bytes;
    uint64_t journal_epoch;
    uint32_t attached;
    uint32_t replay_complete;
    uint16_t rows;
    uint16_t columns;
    char session_id[65];
    char cwd[4096];
    char command[512];
} kpb_wire_status;

#endif

#ifndef KITTY_PTY_BROKER_PROTOCOL_H
#define KITTY_PTY_BROKER_PROTOCOL_H

#include <stdint.h>

#define KPB_PROTOCOL_VERSION 1U
#define KPB_PROTOCOL_MAGIC 0x4b504231U
#define KPB_PROTOCOL_MAX_PAYLOAD (1024U * 1024U)

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
    KPB_FRAME_ACK = 12
};

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

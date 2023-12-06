#ifndef STOMP_H
#define STOMP_H

#define STOMP_PORT 61613

enum stompcmd {
    STOMP_CMD_NONE    = 0,
    STOMP_CMD_CONNECT,
    STOMP_CMD_CONNECTED,
    STOMP_CMD_SEND,
    STOMP_CMD_SUBSCRIBE,
    STOMP_CMD_UNSUBSCRIBE,
    STOMP_CMD_ACK,
    STOMP_CMD_NACK,
    STOMP_CMD_BEGIN,
    STOMP_CMD_COMMIT,
    STOMP_CMD_ABORT,
    STOMP_CMD_DISCONNECT,
    STOMP_CMD_MESSAGE,
    STOMP_CMD_RECEIPT,
    STOMP_CMD_ERROR,
};

struct stomp_cmd {
    enum stompcmd cmd_id;
    const char        *cmd_name;
};
#define STOMP_CMDS { \
    { STOMP_CMD_CONNECT,         "CONNECT" }, \
    { STOMP_CMD_CONNECT,         "STOMP" }, \
    { STOMP_CMD_CONNECTED,     "CONNECTED" }, \
    { STOMP_CMD_SEND,                 "SEND" }, \
    { STOMP_CMD_SUBSCRIBE,     "SUBSCRIBE" }, \
    { STOMP_CMD_UNSUBSCRIBE, "UNSUBSCRIBE" }, \
    { STOMP_CMD_ACK,                 "ACK" },\
    { STOMP_CMD_NACK,                 "NACK" }, \
    { STOMP_CMD_BEGIN,             "BEGIN" }, \
    { STOMP_CMD_COMMIT,             "COMMIT" }, \
    { STOMP_CMD_ABORT,             "ABORT" }, \
    { STOMP_CMD_DISCONNECT,     "DISCONNECT" }, \
    { STOMP_CMD_MESSAGE,         "MESSAGE" }, \
    { STOMP_CMD_RECEIPT,         "RECEIPT" }, \
    { STOMP_CMD_ERROR,             "ERROR" }, \
    { STOMP_CMD_NONE,                  NULL } \
}

struct stomp_descriptor {
    enum stompcmd stomp_cmd;
    struct kvlist stomp_headers;    
};

#endif /* STOMP_H */

#ifndef STOMPD_H
#define STOMPD_H

#define TCP_BACKLOG 10
#define MAX_LOGLINE 300
#define SERVER_MAXHEADERLENGTH 100

enum stompchunk {
    TOREAD_UNLIMITED        = -1,
    TOREAD_STOMP_HEADER    = -2,
    TOREAD_STOMP_TERM        = -3,
    TOREAD_STOMP_NONE        = -4,
};

TAILQ_HEAD (kvlist, kv);
struct kv {
    char *key;
    char *value;

    TAILQ_ENTRY(kv) next;
};

struct session {
    u_int32_t id;
    struct sockaddr_storage clt_ss;
    struct bufferevent *clt_bufev;
    int clt_fd;
    int clt_headersdone;
    int clt_line;
    int clt_done;
    off_t clt_toread;
    struct stomp_descriptor *clt_descreq;

    LIST_ENTRY(session) entry;
};

/* stompd.c */
const char *
    sock_ntop(struct sockaddr *);
void logmsg(int, const char *, ...);
void handle_signal(int, short, void *);
void handle_connection(const int, short, void *);
struct session *
    init_session(void);
void end_session(struct session *);
void reset_session_state(struct session *);
void client_error(struct bufferevent *, short, void *);
void client_read(struct bufferevent *, void *);
void client_readbody(struct bufferevent *, void *);
void client_respond(struct session *);
enum stompcmd stompcmd_byname(const char *);

#endif /* STOMPD_H */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <err.h>
#include <errno.h>
#include <event.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <vis.h>
#include <ctype.h>
#include <limits.h>

#include "stompd.h"
#include "stomp.h"

#define NTOP_BUFS 3

#define sstosa(ss) ((struct sockaddr *)(ss))

static int stompcmd_cmp(const void *, const void *);

struct event listen_ev, pause_accept_ev;
char *listen_ip, *listen_port;
int loglevel, max_sessions, session_count, id_count, timeout;
extern char *__progname;

char ntop_buf[NTOP_BUFS][INET6_ADDRSTRLEN];

LIST_HEAD(, session) sessions = LIST_HEAD_INITIALIZER(sessions);

static struct stomp_cmd stomp_cmds[] = STOMP_CMDS;

void
handle_connection(const int listen_fd, short event, void *arg)
{
    struct sockaddr_storage tmp_ss;
    struct sockaddr *client_sa;
    struct session *s;
    socklen_t len;
    int client_fd;

    event_add(&listen_ev, NULL);

    if ((event & EV_TIMEOUT))
        /* accept() is no longer paused. */
        return;

    /*
     * We _must_ accept the connection, otherwise libevent will keep
     * coming back, and we will chew up all CPU.
     */
    client_sa = sstosa(&tmp_ss);
    len = sizeof(struct sockaddr_storage);
    if ((client_fd = accept(listen_fd, client_sa, &len)) == -1) {
        logmsg(LOG_CRIT, "accept() failed: %s", strerror(errno));

        /*
         * Pause accept if we are out of file descriptors, or
         * libevent will haunt us here too.
         */
         if (errno == ENFILE || errno == EMFILE) {
             struct timeval pause = { 1, 0 };

             event_del(&listen_ev);
             evtimer_add(&pause_accept_ev, &pause);
         } else if (errno != EWOULDBLOCK && errno != EINTR &&
             errno != ECONNABORTED) {
                 logmsg(LOG_CRIT, "accept() failed: %s", strerror(errno));
                 return;
        }
    }
    /* Refuse connection if the maximum is reached. */
    if (session_count >= max_sessions) {
        logmsg(LOG_ERR, "client limit (%d) reached, refusing "
            "connection from %s", max_sessions, sock_ntop(client_sa));
        close(client_fd);
        return;
    }

    /* Allocate session and copy back the info from the accept(). */
    s = init_session();
    if (s == NULL) {
        logmsg(LOG_CRIT, "init_session failed");
        close(client_fd);
        return;
    }
    s->clt_fd = client_fd;
    memcpy(sstosa(&s->clt_ss), client_sa, client_sa->sa_len);

    /* Cast it once, and be done with it. */
    client_sa = sstosa(&s->clt_ss);

    /* Log id/client early to ease debugging. */
    logmsg(LOG_DEBUG, "#%d accepted connection from %s", s->id,
      sock_ntop(client_sa));
    
    /*
     * Setup buffered events.
     */
    s->clt_bufev = bufferevent_new(s->clt_fd, &client_read, NULL,
        &client_error, s);
    if (s->clt_bufev == NULL) {
        logmsg(LOG_CRIT, "#%d bufferevent_new client failed", s->id);
        goto fail;
    }
    bufferevent_settimeout(s->clt_bufev, timeout, 0);
    bufferevent_enable(s->clt_bufev, EV_READ | EV_TIMEOUT);

    return;

fail:
    end_session(s);
}

void
client_error(struct bufferevent *bufev, short what, void *arg)
{
    struct session *s = arg;

    if (what & EVBUFFER_EOF)
        logmsg(LOG_INFO, "#%d client close", s->id);
    else if (what == (EVBUFFER_ERROR | EVBUFFER_READ))
        logmsg(LOG_ERR, "#%d client reset connection", s->id);
    else if (what & EVBUFFER_TIMEOUT)
        logmsg(LOG_ERR, "#%d client timeout", s->id);
    else if (what & EVBUFFER_WRITE)
        logmsg(LOG_ERR, "#%d client write error: %d", s->id, what);
    else
        logmsg(LOG_ERR, "#%d abnormal client error: %d", s->id, what);

    end_session(s);
}

void
client_readbody(struct bufferevent *bufev, void *arg)
{
    struct session *s = arg;
    struct evbuffer    *src = EVBUFFER_INPUT(bufev);
    size_t size, nread;
    char isnull;

    size = EVBUFFER_LENGTH(src);
    logmsg(LOG_DEBUG, "#%d %s: size %lu",
        s->id, __func__, size);
    if (!size) {
        return;
    }

    /* No body to read, just consume the null */
    if (s->clt_toread == TOREAD_STOMP_NONE) {
        nread = evbuffer_remove(src, &isnull, 1);
        if (nread < 1) {
            return;
        }
        
        if (isnull != '\0') {
            logmsg(LOG_DEBUG, "#%d expected null terminator", s->id);            
            end_session(s);
            return;
        }

        logmsg(LOG_DEBUG, "#%d removed null from buffer", s->id);            

        s->clt_toread = 0;
    }

    // TODO: read the body and put it somewhere

    if (s->clt_toread == 0) {
        s->clt_toread = TOREAD_STOMP_HEADER;
        bufferevent_disable(bufev, EV_READ);
        bufev->readcb = client_read;
        return;
    }

    if (bufev->readcb != client_readbody)
        bufev->readcb(bufev, arg);

    return;
}

void
client_read(struct bufferevent *bufev, void *arg)
{
    struct session *s = arg;
    struct evbuffer    *src = EVBUFFER_INPUT(bufev);
    struct stomp_descriptor *desc = s->clt_descreq;
    struct kv *hdr, *tmpkv;
    char *line = NULL, *key = NULL, *value = NULL;
    const char *errstr;
    size_t size, linelen;

    size = EVBUFFER_LENGTH(src);
    logmsg(LOG_DEBUG, "#%d %s: size %lu",
        s->id, __func__, size);
    if (!size) {
        return;
    }

    while (!s->clt_headersdone)
    {
        if (!s->clt_line) {
            /* Peek into the buffer to see if it looks like a STOMP frame */
            key = EVBUFFER_DATA(src);
            // TODO: maybe do a more involved check
            /*
            if (!isalpha((unsigned char)*key)) {
                logmsg(LOG_DEBUG, "#%d %s: invalid frame line", s->id, key);
                goto bad;
            }
            */
        }
        if ((line = evbuffer_readln(src,
            &linelen, EVBUFFER_EOL_CRLF)) == NULL) {
            /* No newline found after too many bytes */
            if (size > SERVER_MAXHEADERLENGTH) {
                logmsg(LOG_DEBUG, "#%s reached max header length", s->id);
                goto bad;
            }
            break;
        }

        /*
         * An empty line indicates the end of the header(s).
         * libevent already stripped the \r\n for us.
         */
        if (!linelen) {
            s->clt_headersdone = 1;
            free(line);
            break;
        }
        key = line;

        logmsg(LOG_DEBUG, "#%d client said: %s", s->id, line);
        
        // TODO: if headers length is too long, goto bad

        /* Get the stomp command */
        if (++s->clt_line == 1) {
            if ((desc->stomp_cmd = stompcmd_byname(key)) == STOMP_CMD_NONE) {
                logmsg(LOG_DEBUG, "#%d %s: command not found", s->id, key);            
                goto bad;
            }
            logmsg(LOG_DEBUG, "#%d cmd: %s", s->id, key);            
        } else {
            /* Get header */
            value = strchr(key, ':');
            if (value == NULL) {
                logmsg(LOG_DEBUG, "#%d invalid header", s->id);
                goto bad;
            }
        }
        
        if (value == NULL)
        {
            free(line);
            continue;
        }

        if (*value == ':') {
            *value++ = '\0';
            // TODO: check if this is within spec
            value += strspn(value, " \t\r\n");
        } else {
            *value++ = '\0';
        }

        if (desc->stomp_cmd != STOMP_CMD_NONE &&
            strcmp("content-length", key) == 0) {
                s->clt_toread = strtonum(value, 0, LLONG_MAX,
                  &errstr);
                if (errstr) {
                    logmsg(LOG_DEBUG, "#%d content-length: %s", s->id, errstr);            
                    goto bad;
                }
        } else if (desc->stomp_cmd != STOMP_CMD_NONE &&
            strcmp("content-type", key) == 0) {
            // TODO: decode mime-type and store for body validation later on
        } else if (desc->stomp_cmd != STOMP_CMD_NONE &&
            strcmp("receipt", key) == 0) {
            if (desc->stomp_cmd == STOMP_CMD_CONNECT) {
                logmsg(LOG_DEBUG, "#%d CONNECT frame MUST not contain \"receipt\" header", s->id);            
                goto bad;
            }
        }

        /* Add stomp header to descriptor */
        // TODO: probably deserves a seperate func
        if (s->clt_line != 1) {
            if ((tmpkv = calloc(1, sizeof(*tmpkv))) == NULL) {
                logmsg(LOG_DEBUG, "#%d header calloc failed", s->id);            
                goto bad;
            }

            if ((tmpkv->key = strdup(key)) == NULL) {
                free(tmpkv);
                goto bad;
            }
            if (value != NULL &&
                (tmpkv->value = strdup(value)) == NULL) {
                    free(tmpkv->key);
                    free(tmpkv);
                    goto bad;
            }        

            TAILQ_INSERT_TAIL(&desc->stomp_headers, tmpkv, next);
        }
            
        free(line);
    }

    if (s->clt_headersdone) {
        logmsg(LOG_DEBUG, "#%d headers done", s->id);            
    
        TAILQ_FOREACH(hdr, &desc->stomp_headers, next)
            logmsg(LOG_DEBUG, "#%d header key=%s, value=%s", s->id, hdr->key, hdr->value);            

        if (desc->stomp_cmd == STOMP_CMD_NONE) {
            logmsg(LOG_DEBUG, "#%d no command", s->id);            
            // TODO: handle error like spec wants
            end_session(s);
            return;
        }

        switch (desc->stomp_cmd) {
        case STOMP_CMD_CONNECT:
        case STOMP_CMD_SUBSCRIBE:
        case STOMP_CMD_UNSUBSCRIBE:
        case STOMP_CMD_ACK:
        case STOMP_CMD_NACK:
        case STOMP_CMD_BEGIN:
        case STOMP_CMD_COMMIT:
        case STOMP_CMD_ABORT:
        case STOMP_CMD_DISCONNECT:
            // TODO: check clt_toread and make sure it's set to either
            // TOREAD_STOMP_HEADER or set to 0 (via content-length header
            // if present, else fail.
            s->clt_toread = TOREAD_STOMP_NONE;
            break;
        case STOMP_CMD_SEND:
        default:
            logmsg(LOG_DEBUG, "#%d command not allowed", s->id);            
            // TODO: handle error like spec wants
            end_session(s);
            return;
        }
        
        bufev->readcb = client_readbody;
        bufferevent_disable(bufev, EV_READ);

        client_respond(s);
    } 

    if (EVBUFFER_LENGTH(src) && bufev->readcb != client_read)
        bufev->readcb(bufev, arg);
    bufferevent_enable(bufev, EV_READ);

    if (s->clt_done) {
        end_session(s);
    }

    return;

bad:
    // TODO: handle error like spec wants
    end_session(s);
    free(line);
    return;
}

void
client_respond(struct session *s)
{
    struct stomp_descriptor *desc = s->clt_descreq;
    char wbuf[256];
    struct kv *hdr;
    char *recval = NULL;
    size_t wbuflen;

    switch (desc->stomp_cmd) {
    case STOMP_CMD_CONNECT: {
        // quick and dirty. no checks
        snprintf(wbuf, sizeof(wbuf),
        "CONNECTED\nversion:1.2\n\n");
        wbuflen = strlen(wbuf);
        bufferevent_write(s->clt_bufev, wbuf, wbuflen + 1);            
        wbuflen = 0;
        wbuf[0] = '\0';
        reset_session_state(s);
        break;
    }
    case STOMP_CMD_SUBSCRIBE:
    case STOMP_CMD_UNSUBSCRIBE:
    case STOMP_CMD_ACK:
    case STOMP_CMD_NACK:
    case STOMP_CMD_BEGIN:
    case STOMP_CMD_COMMIT:
    case STOMP_CMD_ABORT:
    case STOMP_CMD_DISCONNECT: {
        // start: quick and dirty. no checks
        // get the reciept header
        TAILQ_FOREACH(hdr, &desc->stomp_headers, next) {
            if (strcmp("receipt", hdr->key) == 0)
                recval = hdr->value;
        }
        if (recval != NULL) {
            // write receipt            
            snprintf(wbuf, sizeof(wbuf),
            "RECEIPT\nreceipt-id:%s\n\n", recval);
        } else {
            snprintf(wbuf, sizeof(wbuf),
            "RECEIPT\n\n");
        }
        wbuflen = strlen(wbuf);
        bufferevent_write(s->clt_bufev, wbuf, wbuflen + 1);            
        wbuf[0] = '\0';
        // end: quick and dirty
        // TODO: need to wait a couple milli before closing
        s->clt_done = 1;
        break;
    }
    case STOMP_CMD_SEND:
        // TODO: do send logic
        // TODO: fallthrough to here and check for receipt header??
        // if exist then send a receipt frame
    default:
        logmsg(LOG_DEBUG, "#%d command not allowed");            
        end_session(s);
        return;
    }

    return;
}

struct session *
init_session(void)
{
    struct session *s;
    struct stomp_descriptor *desc;

    s = calloc(1, sizeof(struct session));
    if (s == NULL)
        return (NULL);

    desc = calloc(1, sizeof(struct stomp_descriptor));
    if (desc == NULL)
        return (NULL);

    s->id = id_count++;
    s->clt_fd = -1;
    s->clt_headersdone = 0;
    s->clt_line = 0;
    s->clt_done = 0;
    TAILQ_INIT(&desc->stomp_headers);
    s->clt_descreq = desc;

    LIST_INSERT_HEAD(&sessions, s, entry);
    session_count++;

    return (s);
}

void
reset_session_state(struct session *s)
{
    struct kv *hdr;
    struct stomp_descriptor *desc = s->clt_descreq;

    logmsg(LOG_INFO, "#%d reset session", s->id);

    s->clt_headersdone = 0;
    s->clt_line = 0;
    desc->stomp_cmd = 0;
    while ((hdr = TAILQ_FIRST(&desc->stomp_headers))) {
        TAILQ_REMOVE(&desc->stomp_headers, hdr, next);
        free(hdr);
    }

    return;
}

void
end_session(struct session *s)
{
    struct kv *hdr;
    struct stomp_descriptor *desc = s->clt_descreq;

    logmsg(LOG_INFO, "#%d ending session", s->id);

    /* Flush output buffers. */
    if (s->clt_bufev && s->clt_fd != -1)
        evbuffer_write(s->clt_bufev->output, s->clt_fd);

    if (s->clt_fd != -1)
        close(s->clt_fd);

    if (s->clt_bufev)
        bufferevent_free(s->clt_bufev);

    while ((hdr = TAILQ_FIRST(&desc->stomp_headers))) {
        TAILQ_REMOVE(&desc->stomp_headers, hdr, next);
        free(hdr);
    }
    LIST_REMOVE(s, entry);
    free(s);
    session_count--;
}

enum stompcmd
stompcmd_byname(const char *name)
{
    enum stompcmd id = STOMP_CMD_NONE;
    struct stomp_cmd cmd, *res = NULL;

    /* Set up key */
    cmd.cmd_name = name;

    if ((res = bsearch(&cmd, stomp_cmds,
        sizeof(stomp_cmds) / sizeof(stomp_cmds[0]) - 1,
        sizeof(stomp_cmds[0]), stompcmd_cmp)) != NULL)
        id = res->cmd_id;

    return (id);
}

static int
stompcmd_cmp(const void *a, const void *b)
{
    const struct stomp_cmd *ma = a;
    const struct stomp_cmd *mb = b;

    /*
     * All commands are case sensitive so we don't do a strcasecmp here.
     */
    return (strcmp(ma->cmd_name, mb->cmd_name));
}

int
main(int argc, char *argv[])
{
    struct addrinfo hints, *res;
    struct event ev_sighup, ev_sigint, ev_sigterm;
    int error, listenfd, on;

    listen_ip = "127.0.0.1";
    listen_port = "8080";
    loglevel = LOG_DEBUG; 
    max_sessions = 10;
    timeout = 24 * 3600;

    /* Sort the STOMP lookup array */
    qsort(stomp_cmds, sizeof(stomp_cmds) /
        sizeof(stomp_cmds[0]) - 1,
        sizeof(stomp_cmds[0]), stompcmd_cmp);

    memset(&hints, 0, sizeof hints);
    memset(&res, 0, sizeof res);
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    error = getaddrinfo(listen_ip, listen_port, &hints, &res);    
    if (error)
        errx(1, "getaddrinfo listen address failed: %s",
            gai_strerror(error));
    if ((listenfd = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP)) == -1)
    errx(1, "socket failed");
    on = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (void *)&on,
        sizeof on) != 0)
            err(1, "setsockopt failed");
    if (bind(listenfd, (struct sockaddr *)res->ai_addr,
        (socklen_t)res->ai_addrlen) != 0)
            err(1, "bind failed");
    if (listen(listenfd, TCP_BACKLOG) != 0)
        err(1, "listen failed");
    freeaddrinfo(res);

    openlog(__progname, LOG_PID | LOG_NDELAY, LOG_DAEMON);

    (void)event_init();

    /* Setup signal handler. */
    signal(SIGPIPE, SIG_IGN);
    signal_set(&ev_sighup, SIGHUP, handle_signal, NULL);
    signal_set(&ev_sigint, SIGINT, handle_signal, NULL);
    signal_set(&ev_sigterm, SIGTERM, handle_signal, NULL);
    signal_add(&ev_sighup, NULL);
    signal_add(&ev_sigint, NULL);
    signal_add(&ev_sigterm, NULL);

    event_set(&listen_ev, listenfd, EV_READ, handle_connection, NULL);
    event_add(&listen_ev, NULL);
    evtimer_set(&pause_accept_ev, handle_connection, NULL);

    logmsg(LOG_NOTICE, "listening on %s port %s", listen_ip, listen_port);

    /*  Vroom, vroom.  */
    event_dispatch();

    logmsg(LOG_ERR, "event_dispatch error: %s", strerror(errno));

    /* NOTREACHED */
    return (1);
}

const char *
sock_ntop(struct sockaddr *sa)
{
    static int n = 0;

    /* Cycle to next buffer. */
    n = (n + 1) % NTOP_BUFS;
    ntop_buf[n][0] = '\0';

    if (sa->sa_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)sa;

        return (inet_ntop(AF_INET, &sin->sin_addr, ntop_buf[n],
            sizeof ntop_buf[0]));
    }

    if (sa->sa_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sa;

        return (inet_ntop(AF_INET6, &sin6->sin6_addr, ntop_buf[n],
            sizeof ntop_buf[0]));
    }

    return (NULL);
}

void
logmsg(int pri, const char *message, ...)
{
    va_list ap;

    if (pri > loglevel)
      return;

    va_start(ap, message);

    char buf[MAX_LOGLINE];
    char visbuf[2 * MAX_LOGLINE];

    /* We don't care about truncation. */
    vsnprintf(buf, sizeof buf, message, ap);
    strnvis(visbuf, buf, sizeof visbuf, VIS_CSTYLE | VIS_NL);
    fprintf(stderr, "%s\n", visbuf);

    va_end(ap);
}

void
handle_signal(int sig, short event, void *arg)
{

    /*
     * Signal handler rules don't apply, libevent decouples for us.
     */
    logmsg(LOG_ERR, "exiting on signal %d", sig);

    closelog();
    exit(0);
}

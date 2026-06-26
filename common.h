#ifndef COMMON_H
#define COMMON_H

/* =========================================================
 * common.h  –  Shared definitions for Lost & Found System
 * ========================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mqueue.h>
#include <stdarg.h>

/* ── Network ─────────────────────────────────────────────── */
#define SERVER_PORT       8080
#define SERVER_BACKLOG    10
#define BUFFER_SIZE       2048

/* ── Files ───────────────────────────────────────────────── */
#define ITEMS_FILE        "items.dat"
#define USERS_FILE        "users.dat"

/* ── POSIX Message Queue ─────────────────────────────────── */
#define MQ_NEW_FOUND      "/laf_new_found"   /* server  → matcher */
#define MQ_MATCH_RESULT   "/laf_match_result"/* matcher → server  */
#define MQ_MAX_MSG        10
#define MQ_MSG_SIZE       256

/* ── Roles ───────────────────────────────────────────────── */
typedef enum { ROLE_GUEST = 0, ROLE_USER = 1, ROLE_ADMIN = 2 } Role;
static const char *ROLE_NAMES[] = { "GUEST", "USER", "ADMIN" };

/* ── Item status ─────────────────────────────────────────── */
typedef enum {
    STATUS_LOST    = 0,
    STATUS_FOUND   = 1,
    STATUS_MATCHED = 2,
    STATUS_CLAIMED = 3
} ItemStatus;
static const char *STATUS_NAMES[] = { "LOST", "FOUND", "MATCHED", "CLAIMED" };

/* ── Item record (stored in items.dat) ───────────────────── */
#define DESC_LEN   128
#define LOC_LEN     64
#define NAME_LEN    48

typedef struct {
    int        id;
    ItemStatus status;
    char       description[DESC_LEN];
    char       location[LOC_LEN];
    char       posted_by[NAME_LEN];
    time_t     timestamp;
    int        active;   /* 1 = valid record, 0 = deleted */
} Item;

/* ── User record (stored in users.dat) ──────────────────── */
#define PASS_LEN    32

typedef struct {
    char username[NAME_LEN];
    char password[PASS_LEN];
    Role role;
    int  active;
} User;

/* ── IPC message sent through POSIX MQ ──────────────────── */
typedef struct {
    int  item_id;
    char description[DESC_LEN];
    char location[LOC_LEN];
} MQMessage;

/* ── Pending match (shared memory / mutex-protected list) ── */
#define MAX_PENDING  64

typedef struct {
    int lost_id;
    int found_id;
    int active;
} PendingMatch;

/* ── Shared state (server-side, guarded by mutexes) ─────── */
typedef struct {
    PendingMatch  pending[MAX_PENDING];
    int           pending_count;
    pthread_mutex_t pending_mutex;

    int           active_users;
    pthread_mutex_t users_mutex;
} SharedState;

/* ── Per-session context passed to each handler thread ─── */
typedef struct {
    int         sock_fd;
    char        client_ip[INET_ADDRSTRLEN];
    int         client_port;
    char        username[NAME_LEN];
    Role        role;
    SharedState *shared;
} Session;

/* ── Protocol command tokens (client ↔ server) ───────────── */
#define CMD_LOGIN       "LOGIN"
#define CMD_LOGOUT      "LOGOUT"
#define CMD_LIST_LOST   "LIST_LOST"
#define CMD_LIST_FOUND  "LIST_FOUND"
#define CMD_LIST_MATCH  "LIST_MATCH"
#define CMD_POST_LOST   "POST_LOST"
#define CMD_POST_FOUND  "POST_FOUND"
#define CMD_APPROVE     "APPROVE"
#define CMD_DELETE      "DELETE"
#define CMD_GUEST       "GUEST"
#define CMD_REGISTER    "REGISTER"
#define CMD_VIEW_USERS  "VIEW_USERS"

/* ── Response tokens ─────────────────────────────────────── */
#define RESP_OK         "OK"
#define RESP_ERR        "ERR"
#define RESP_DENIED     "DENIED"
#define RESP_END        "END"

/* ── Helpers ─────────────────────────────────────────────── */
static inline void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static inline void log_ts(const char *tag, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm);
    printf("[%s] %s %s\n", ts, tag, buf);
    fflush(stdout);
}

#endif /* COMMON_H */

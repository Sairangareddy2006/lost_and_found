/* =========================================================
 * server.c  –  Main Server Process
 *
 * Responsibilities:
 *   • Accept TCP connections; spawn a handler thread per client
 *   • Role-based authorisation (Guest / User / Admin)
 *   • File I/O with fcntl() advisory locking on items.dat
 *   • Mutex-protected pending-match queue (shared with Matcher
 *     via POSIX MQ result channel)
 *   • Seed default users on first run
 * ========================================================= */

#include "common.h"
#include <stdarg.h>

/* ── Forward declarations ─────────────────────────────────── */
static void  seed_default_data(void);
static void *handle_client(void *arg);
static void  cmd_login(Session *s, const char *args);
static void  cmd_list(Session *s, ItemStatus filter);
static void  cmd_post(Session *s, ItemStatus type, const char *args);
static void  cmd_approve(Session *s, const char *args);
static void  cmd_delete(Session *s, const char *args);
static void  cmd_list_pending(Session *s);
static void  cmd_register(Session *s, const char *args);
static void  cmd_view_users(Session *s);
static void  send_msg(int fd, const char *fmt, ...);
static int   recv_line(int fd, char *buf, int maxlen);

/* ── IPC: background thread that drains the match-result MQ ─ */
static void *mq_result_reader(void *arg);

/* ── Global shared state ─────────────────────────────────── */
static SharedState g_shared;
static mqd_t       g_mq_found;   /* server writes NEW_FOUND items  */
static mqd_t       g_mq_result;  /* server reads  MATCH_RESULT      */

/* ================================================================
 * main()
 * ================================================================ */
int main(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║      CONCURRENT LOST & FOUND SERVER v1.0         ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    /* ── Initialise shared state ──────────────────────────── */
    memset(&g_shared, 0, sizeof(g_shared));
    if (pthread_mutex_init(&g_shared.pending_mutex, NULL) != 0)
        die("pthread_mutex_init(pending)");
    if (pthread_mutex_init(&g_shared.users_mutex, NULL) != 0)
        die("pthread_mutex_init(users)");
    log_ts("[MUTEX]", "Initialised pending_mutex and users_mutex.");

    /* ── Seed default users / items if files are missing ─── */
    seed_default_data();

    /* ── Open POSIX message queues ───────────────────────── */
    struct mq_attr attr = {
        .mq_flags   = 0,
        .mq_maxmsg  = MQ_MAX_MSG,
        .mq_msgsize = MQ_MSG_SIZE,
        .mq_curmsgs = 0
    };

    /* Remove stale queues from a previous crash */
    mq_unlink(MQ_NEW_FOUND);
    mq_unlink(MQ_MATCH_RESULT);

    g_mq_found = mq_open(MQ_NEW_FOUND, O_CREAT | O_WRONLY, 0644, &attr);
    if (g_mq_found == (mqd_t)-1) die("mq_open(MQ_NEW_FOUND)");
    log_ts("[IPC]", "Message queue '%s' created for NEW FOUND items.", MQ_NEW_FOUND);

    g_mq_result = mq_open(MQ_MATCH_RESULT, O_CREAT | O_RDONLY, 0644, &attr);
    if (g_mq_result == (mqd_t)-1) die("mq_open(MQ_MATCH_RESULT)");
    log_ts("[IPC]", "Message queue '%s' created for MATCH RESULTS.", MQ_MATCH_RESULT);

    /* ── Spawn IPC result-reader thread ──────────────────── */
    pthread_t mq_tid;
    pthread_create(&mq_tid, NULL, mq_result_reader, NULL);
    pthread_detach(mq_tid);
    log_ts("[IPC]", "Background MQ result-reader thread spawned (tid=%lu).", (unsigned long)mq_tid);

    /* ── Create TCP listening socket ─────────────────────── */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) die("socket()");

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(SERVER_PORT)
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("bind()");
    if (listen(listen_fd, SERVER_BACKLOG) < 0)
        die("listen()");

    log_ts("[SOCKET]", "Server listening on port %d  (backlog=%d).", SERVER_PORT, SERVER_BACKLOG);
    printf("\n  Default credentials:\n");
    printf("  ┌──────────┬──────────┬────────┐\n");
    printf("  │ Username │ Password │ Role   │\n");
    printf("  ├──────────┼──────────┼────────┤\n");
    printf("  │ admin    │ admin123 │ ADMIN  │\n");
    printf("  │ alice    │ pass123  │ USER   │\n");
    printf("  │ bob      │ pass456  │ USER   │\n");
    printf("  └──────────┴──────────┴────────┘\n\n");

    /* ── Accept loop ─────────────────────────────────────── */
    for (;;) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);

        int cli_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (cli_fd < 0) {
            log_ts("[SOCKET]", "accept() failed: %s", strerror(errno));
            continue;
        }

        Session *s = calloc(1, sizeof(Session));
        s->sock_fd     = cli_fd;
        s->role        = ROLE_GUEST;
        s->shared      = &g_shared;
        s->client_port = ntohs(cli_addr.sin_port);
        inet_ntop(AF_INET, &cli_addr.sin_addr, s->client_ip, INET_ADDRSTRLEN);

        log_ts("[SOCKET]", "Client connected from %s:%d  (fd=%d).",
               s->client_ip, s->client_port, cli_fd);

        pthread_mutex_lock(&g_shared.users_mutex);
        g_shared.active_users++;
        log_ts("[MUTEX]", "users_mutex LOCKED – active users: %d.", g_shared.active_users);
        pthread_mutex_unlock(&g_shared.users_mutex);
        log_ts("[MUTEX]", "users_mutex UNLOCKED.");

        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, s);
        pthread_detach(tid);
        log_ts("[SOCKET]", "Handler thread spawned for %s:%d (tid=%lu).",
               s->client_ip, s->client_port, (unsigned long)tid);
    }

    mq_close(g_mq_found);
    mq_close(g_mq_result);
    mq_unlink(MQ_NEW_FOUND);
    mq_unlink(MQ_MATCH_RESULT);
    pthread_mutex_destroy(&g_shared.pending_mutex);
    pthread_mutex_destroy(&g_shared.users_mutex);
    close(listen_fd);
    return 0;
}

/* ================================================================
 * MQ result-reader thread
 *   – runs forever, receives match notifications from Matcher
 *   – inserts them into the mutex-protected pending queue
 * ================================================================ */
static void *mq_result_reader(void *arg) {
    (void)arg;
    char raw[MQ_MSG_SIZE + 1];
    log_ts("[IPC]", "MQ result-reader thread STARTED.");

    for (;;) {
        ssize_t n = mq_receive(g_mq_result, raw, MQ_MSG_SIZE, NULL);
        if (n < 0) {
            log_ts("[IPC]", "mq_receive(result) error: %s", strerror(errno));
            sleep(1);
            continue;
        }
        raw[n] = '\0';

        /* Format: "lost_id:found_id" */
        int lost_id = 0, found_id = 0;
        if (sscanf(raw, "%d:%d", &lost_id, &found_id) != 2) {
            log_ts("[IPC]", "Malformed match result message: '%s'", raw);
            continue;
        }

        log_ts("[IPC]", "Match result received from Matcher: LOST #%d ↔ FOUND #%d.", lost_id, found_id);

        /* Insert into pending queue under mutex */
        pthread_mutex_lock(&g_shared.pending_mutex);
        log_ts("[MUTEX]", "pending_mutex LOCKED by MQ-reader thread – adding pending match.");

        if (g_shared.pending_count < MAX_PENDING) {
            /* Find an empty slot */
            for (int i = 0; i < MAX_PENDING; i++) {
                if (!g_shared.pending[i].active) {
                    g_shared.pending[i].lost_id  = lost_id;
                    g_shared.pending[i].found_id = found_id;
                    g_shared.pending[i].active   = 1;
                    g_shared.pending_count++;
                    log_ts("[RACE-PREVENTION]", "Pending match slot %d secured safely under mutex.", i);
                    break;
                }
            }
        } else {
            log_ts("[IPC]", "WARNING: Pending match queue FULL (%d/%d). Match LOST #%d/FOUND #%d dropped.",
                   g_shared.pending_count, MAX_PENDING, lost_id, found_id);
        }

        pthread_mutex_unlock(&g_shared.pending_mutex);
        log_ts("[MUTEX]", "pending_mutex UNLOCKED by MQ-reader thread.");
    }
    return NULL;
}

/* ================================================================
 * handle_client()  – per-connection handler thread
 * ================================================================ */
static void *handle_client(void *arg) {
    Session *s = (Session *)arg;
    char buf[BUFFER_SIZE];

    send_msg(s->sock_fd,
        "WELCOME Lost&Found System | Commands: GUEST | LOGIN <user> <pass> | QUIT\n");

    while (recv_line(s->sock_fd, buf, sizeof(buf)) > 0) {
        /* Trim trailing newline/CR */
        buf[strcspn(buf, "\r\n")] = '\0';
        if (strlen(buf) == 0) continue;

        log_ts("[SOCKET]", "Received from %s:%d → '%s'",
               s->client_ip, s->client_port, buf);

        if (strncmp(buf, CMD_GUEST, 5) == 0) {
            s->role = ROLE_GUEST;
            strncpy(s->username, "guest", NAME_LEN - 1);
            log_ts("[AUTH]", "Guest session started for %s:%d.", s->client_ip, s->client_port);
            send_msg(s->sock_fd, "OK Logged in as GUEST (read-only)\n");

        } else if (strncmp(buf, CMD_LOGIN, 5) == 0) {
            cmd_login(s, buf + 6);

        } else if (strncmp(buf, CMD_REGISTER, 8) == 0) {
            cmd_register(s, buf + 9);

        } else if (strncmp(buf, CMD_LOGOUT, 6) == 0) {
            send_msg(s->sock_fd, "OK Goodbye, %s\n", s->username);
            s->role = ROLE_GUEST;
            strcpy(s->username, "");
            log_ts("[AUTH]", "User logged out at %s:%d.", s->client_ip, s->client_port);

        } else if (strncmp(buf, CMD_LIST_LOST, 9) == 0) {
            cmd_list(s, STATUS_LOST);

        } else if (strncmp(buf, CMD_LIST_FOUND, 10) == 0) {
            cmd_list(s, STATUS_FOUND);

        } else if (strncmp(buf, CMD_LIST_MATCH, 10) == 0) {
            cmd_list_pending(s);

        } else if (strncmp(buf, CMD_POST_LOST, 9) == 0) {
            if (s->role < ROLE_USER) {
                log_ts("[AUTH]", "Access DENIED for %s (role=%s): POST_LOST requires USER.",
                       s->username, ROLE_NAMES[s->role]);
                send_msg(s->sock_fd, "DENIED Only logged-in users can post items.\n");
            } else {
                cmd_post(s, STATUS_LOST, buf + 10);
            }

        } else if (strncmp(buf, CMD_POST_FOUND, 10) == 0) {
            if (s->role < ROLE_USER) {
                log_ts("[AUTH]", "Access DENIED for %s (role=%s): POST_FOUND requires USER.",
                       s->username, ROLE_NAMES[s->role]);
                send_msg(s->sock_fd, "DENIED Only logged-in users can post items.\n");
            } else {
                cmd_post(s, STATUS_FOUND, buf + 11);
            }

        } else if (strncmp(buf, CMD_APPROVE, 7) == 0) {
            if (s->role < ROLE_ADMIN) {
                log_ts("[AUTH]", "Access DENIED for %s (role=%s): APPROVE requires ADMIN.",
                       s->username, ROLE_NAMES[s->role]);
                send_msg(s->sock_fd, "DENIED Only admins can approve matches.\n");
            } else {
                cmd_approve(s, buf + 8);
            }

        } else if (strncmp(buf, CMD_DELETE, 6) == 0) {
            if (s->role < ROLE_ADMIN) {
                log_ts("[AUTH]", "Access DENIED for %s (role=%s): DELETE requires ADMIN.",
                       s->username, ROLE_NAMES[s->role]);
                send_msg(s->sock_fd, "DENIED Only admins can delete records.\n");
            } else {
                cmd_delete(s, buf + 7);
            }

        } else if (strcmp(buf, CMD_VIEW_USERS) == 0) {
            cmd_view_users(s);

        } else if (strcmp(buf, "QUIT") == 0) {
            send_msg(s->sock_fd, "OK Closing connection.\n");
            break;

        } else if (strcmp(buf, "HELP") == 0) {
            send_msg(s->sock_fd,
                "Commands:\n"
                "  GUEST                          – read-only access\n"
                "  LOGIN <user> <pass>            – authenticate\n"
                "  REGISTER <user> <pass>         – create a new USER account\n"
                "  LOGOUT                         – de-authenticate\n"
                "  LIST_LOST                      – list all lost items\n"
                "  LIST_FOUND                     – list all found items\n"
                "  LIST_MATCH                     – list pending matches (Admin)\n"
                "  POST_LOST <desc>|<location>    – report a lost item (User+)\n"
                "  POST_FOUND <desc>|<location>   – report a found item (User+)\n"
                "  APPROVE <lost_id> <found_id>   – approve a match (Admin)\n"
                "  DELETE <item_id>               – delete a record (Admin)\n"
                "  VIEW_USERS                     – list all registered users (Admin)\n"
                "  QUIT                           – disconnect\n"
                "END\n");

        } else {
            send_msg(s->sock_fd, "ERR Unknown command. Type HELP.\n");
        }
    }

    log_ts("[SOCKET]", "Client %s:%d disconnected. Closing fd=%d.",
           s->client_ip, s->client_port, s->sock_fd);
    close(s->sock_fd);

    pthread_mutex_lock(&g_shared.users_mutex);
    g_shared.active_users--;
    log_ts("[MUTEX]", "users_mutex LOCKED – active users now: %d.", g_shared.active_users);
    pthread_mutex_unlock(&g_shared.users_mutex);
    log_ts("[MUTEX]", "users_mutex UNLOCKED.");

    free(s);
    return NULL;
}

/* ================================================================
 * cmd_login()
 * ================================================================ */
static void cmd_login(Session *s, const char *args) {
    char uname[NAME_LEN] = {0}, pass[PASS_LEN] = {0};
    if (sscanf(args, "%47s %31s", uname, pass) != 2) {
        send_msg(s->sock_fd, "ERR Usage: LOGIN <username> <password>\n");
        return;
    }

    /* Read-lock users.dat */
    int fd = open(USERS_FILE, O_RDONLY);
    if (fd < 0) { send_msg(s->sock_fd, "ERR Internal error (users file).\n"); return; }

    struct flock fl = { .l_type = F_RDLCK, .l_whence = SEEK_SET };
    log_ts("[LOCK]", "Attempting READ lock on %s for LOGIN...", USERS_FILE);
    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        close(fd); send_msg(s->sock_fd, "ERR Lock failed.\n"); return;
    }
    log_ts("[LOCK]", "READ lock ACQUIRED on %s.", USERS_FILE);

    User u;
    int found = 0;
    while (read(fd, &u, sizeof(u)) == sizeof(u)) {
        if (u.active &&
            strcmp(u.username, uname) == 0 &&
            strcmp(u.password, pass)  == 0) {
            found = 1;
            break;
        }
    }

    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    log_ts("[LOCK]", "READ lock RELEASED on %s.", USERS_FILE);
    close(fd);

    if (found) {
        strncpy(s->username, u.username, NAME_LEN - 1);
        s->role = u.role;
        log_ts("[AUTH]", "LOGIN SUCCESS: '%s' authenticated as %s from %s:%d.",
               s->username, ROLE_NAMES[s->role], s->client_ip, s->client_port);
        send_msg(s->sock_fd, "OK Welcome, %s! Role: %s\n",
                 s->username, ROLE_NAMES[s->role]);
    } else {
        log_ts("[AUTH]", "LOGIN FAILED for user '%s' from %s:%d.", uname, s->client_ip, s->client_port);
        send_msg(s->sock_fd, "ERR Invalid username or password.\n");
    }
}

/* ================================================================
 * cmd_register()  – create a new USER account
 *
 * Protocol:  REGISTER <username> <password>
 *
 * OS concepts exercised:
 *   • fcntl() WRITE lock on users.dat  (exclusive – no concurrent
 *     reads or writes allowed during the check-then-append)
 *   • Duplicate-username check performed under the same lock so
 *     two simultaneous registrations of the same name cannot both
 *     succeed (classic TOCTOU prevention)
 * ================================================================ */
static void cmd_register(Session *s, const char *args) {
    char uname[NAME_LEN] = {0}, pass[PASS_LEN] = {0};

    if (sscanf(args, "%47s %31s", uname, pass) != 2) {
        send_msg(s->sock_fd, "ERR Usage: REGISTER <username> <password>\n");
        return;
    }

    /* Basic sanity checks on the supplied values */
    if (strlen(uname) < 3) {
        send_msg(s->sock_fd, "ERR Username must be at least 3 characters.\n");
        return;
    }
    if (strlen(pass) < 4) {
        send_msg(s->sock_fd, "ERR Password must be at least 4 characters.\n");
        return;
    }
    /* Disallow whitespace inside username/password (would break the
     * space-delimited protocol wire format) */
    for (int i = 0; uname[i]; i++) {
        if (uname[i] == ' ' || uname[i] == '\t') {
            send_msg(s->sock_fd, "ERR Username must not contain spaces.\n");
            return;
        }
    }

    log_ts("[AUTH]", "REGISTER request for username='%s' from %s:%d.",
           uname, s->client_ip, s->client_port);

    /*
     * Open users.dat for READ+WRITE (create if it does not exist).
     * We need O_RDWR so we can scan existing records and then append
     * without closing and reopening the file – which would create a
     * window between the duplicate check and the write.
     */
    int fd = open(USERS_FILE, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        log_ts("[LOCK]", "REGISTER: failed to open %s: %s", USERS_FILE, strerror(errno));
        send_msg(s->sock_fd, "ERR Internal error (cannot open users file).\n");
        return;
    }

    /*
     * Acquire a STRICT WRITE LOCK over the entire file.
     *
     * Why a write lock and not a read lock for the scan phase?
     * Because the check and the append must be atomic with respect
     * to every other thread.  If two threads both acquired read
     * locks, scanned, found the username absent, then raced to
     * upgrade to a write lock, the same username could be inserted
     * twice.  A single write lock from the start prevents that.
     */
    struct flock fl = {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0   /* 0 = lock entire file */
    };

    log_ts("[LOCK]", "Attempting WRITE lock on %s for REGISTER (user='%s')...",
           USERS_FILE, uname);

    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        log_ts("[LOCK]", "WRITE lock FAILED on %s: %s", USERS_FILE, strerror(errno));
        close(fd);
        send_msg(s->sock_fd, "ERR Server lock failure – try again.\n");
        return;
    }

    log_ts("[LOCK]", "WRITE lock ACQUIRED on %s for REGISTER (user='%s').", USERS_FILE, uname);
    log_ts("[RACE-PREVENTION]",
           "Exclusive write lock held – concurrent registrations for '%s' "
           "will block until this transaction completes.", uname);

    /* ── Scan for duplicate username ────────────────────── */
    User u;
    int duplicate = 0;
    while (read(fd, &u, sizeof(u)) == sizeof(u)) {
        if (u.active && strcmp(u.username, uname) == 0) {
            duplicate = 1;
            break;
        }
    }

    if (duplicate) {
        /* Release lock before returning */
        fl.l_type = F_UNLCK;
        fcntl(fd, F_SETLK, &fl);
        log_ts("[LOCK]",  "WRITE lock RELEASED on %s (duplicate detected).", USERS_FILE);
        close(fd);

        log_ts("[AUTH]", "REGISTER DENIED: username '%s' already exists.", uname);
        send_msg(s->sock_fd, "ERR Username '%s' is already taken. Choose another.\n", uname);
        return;
    }

    /* ── Append the new user record ─────────────────────── */
    User new_user;
    memset(&new_user, 0, sizeof(new_user));
    strncpy(new_user.username, uname, NAME_LEN - 1);
    strncpy(new_user.password, pass,  PASS_LEN - 1);
    new_user.role   = ROLE_USER;   /* all self-registered accounts are USER */
    new_user.active = 1;

    /* Seek to end of file so we append rather than overwrite */
    lseek(fd, 0, SEEK_END);

    if (write(fd, &new_user, sizeof(new_user)) != (ssize_t)sizeof(new_user)) {
        fl.l_type = F_UNLCK;
        fcntl(fd, F_SETLK, &fl);
        log_ts("[LOCK]", "WRITE lock RELEASED on %s (write error).", USERS_FILE);
        close(fd);

        log_ts("[AUTH]", "REGISTER FAILED: disk write error for user '%s'.", uname);
        send_msg(s->sock_fd, "ERR Failed to save new user – disk error.\n");
        return;
    }

    /* ── Release lock ───────────────────────────────────── */
    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    log_ts("[LOCK]", "WRITE lock RELEASED on %s after successful REGISTER.", USERS_FILE);
    close(fd);

    log_ts("[AUTH]", "REGISTER SUCCESS: new USER account '%s' created from %s:%d.",
           uname, s->client_ip, s->client_port);
    send_msg(s->sock_fd, "OK Account '%s' created with role USER. You can now LOGIN.\n", uname);
}

/* ================================================================
 * cmd_list()  – list LOST or FOUND items (read-locked)
 * ================================================================ */
static void cmd_list(Session *s, ItemStatus filter) {
    int fd = open(ITEMS_FILE, O_RDONLY);
    if (fd < 0) { send_msg(s->sock_fd, "ERR No items file found.\n"); return; }

    struct flock fl = { .l_type = F_RDLCK, .l_whence = SEEK_SET };
    log_ts("[LOCK]", "Attempting READ lock on %s for LIST (filter=%s)...",
           ITEMS_FILE, STATUS_NAMES[filter]);
    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        close(fd); send_msg(s->sock_fd, "ERR Lock failed.\n"); return;
    }
    log_ts("[LOCK]", "READ lock ACQUIRED on %s (user='%s').", ITEMS_FILE, s->username);
    log_ts("[RACE-PREVENTION]", "Concurrent list reads are SAFE – multiple threads may hold read locks simultaneously.");

    Item item;
    int count = 0;
    send_msg(s->sock_fd, "OK\n");
    while (read(fd, &item, sizeof(item)) == sizeof(item)) {
        if (item.active && item.status == filter) {
            char ts[32];
            struct tm *tm = localtime(&item.timestamp);
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", tm);
            send_msg(s->sock_fd,
                "ID:%-4d  Status:%-8s  By:%-12s  Date:%s  Loc:%-20s  Desc:%s\n",
                item.id, STATUS_NAMES[item.status], item.posted_by,
                ts, item.location, item.description);
            count++;
        }
    }
    if (count == 0)
        send_msg(s->sock_fd, "(No records found)\n");

    send_msg(s->sock_fd, "END\n");

    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    log_ts("[LOCK]", "READ lock RELEASED on %s.", ITEMS_FILE);
    close(fd);
}

/* ================================================================
 * cmd_post()  – add LOST or FOUND item (write-locked)
 * ================================================================ */
static void cmd_post(Session *s, ItemStatus type, const char *args) {
    /* args format:  "description|location" */
    char desc[DESC_LEN] = {0}, loc[LOC_LEN] = {0};
    char tmp[BUFFER_SIZE];
    strncpy(tmp, args, sizeof(tmp) - 1);

    char *pipe_pos = strchr(tmp, '|');
    if (!pipe_pos) {
        send_msg(s->sock_fd, "ERR Format: POST_LOST <description>|<location>\n");
        return;
    }
    *pipe_pos = '\0';
    strncpy(desc, tmp,         DESC_LEN - 1);
    strncpy(loc,  pipe_pos+1, LOC_LEN  - 1);

    /* Trim leading spaces */
    char *d = desc; while (*d == ' ') d++;
    char *l = loc;  while (*l == ' ') l++;

    /* Open for read+write; create if absent */
    int fd = open(ITEMS_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { send_msg(s->sock_fd, "ERR Cannot open items file.\n"); return; }

    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    log_ts("[LOCK]", "Attempting WRITE lock on %s for POST_%s by '%s'...",
           ITEMS_FILE, STATUS_NAMES[type], s->username);
    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        close(fd); send_msg(s->sock_fd, "ERR Lock failed.\n"); return;
    }
    log_ts("[LOCK]", "WRITE lock ACQUIRED on %s by thread (user='%s').", ITEMS_FILE, s->username);
    log_ts("[RACE-PREVENTION]", "Exclusive write lock prevents concurrent modifications to items.dat.");

    /* Determine next ID */
    int max_id = 0;
    Item item;
    while (read(fd, &item, sizeof(item)) == sizeof(item)) {
        if (item.active && item.id > max_id)
            max_id = item.id;
    }

    Item new_item = {0};
    new_item.id        = max_id + 1;
    new_item.status    = type;
    new_item.timestamp = time(NULL);
    new_item.active    = 1;
    strncpy(new_item.description, d, DESC_LEN - 1);
    strncpy(new_item.location,    l, LOC_LEN  - 1);
    strncpy(new_item.posted_by,   s->username, NAME_LEN - 1);

    lseek(fd, 0, SEEK_END);
    if (write(fd, &new_item, sizeof(new_item)) != sizeof(new_item)) {
        fl.l_type = F_UNLCK; fcntl(fd, F_SETLK, &fl); close(fd);
        send_msg(s->sock_fd, "ERR Write failed.\n");
        return;
    }

    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    log_ts("[LOCK]", "WRITE lock RELEASED on %s.", ITEMS_FILE);
    close(fd);

    log_ts("[SOCKET]", "New %s item #%d posted by '%s': '%s' @ '%s'.",
           STATUS_NAMES[type], new_item.id, s->username, d, l);

    /* If this is a FOUND item, send it to the Matcher via MQ */
    if (type == STATUS_FOUND) {
        char mq_buf[MQ_MSG_SIZE];
        snprintf(mq_buf, sizeof(mq_buf), "%d|%s|%s", new_item.id, d, l);

        log_ts("[IPC]", "Server sending FOUND item #%d to message queue '%s'.",
               new_item.id, MQ_NEW_FOUND);
        if (mq_send(g_mq_found, mq_buf, strlen(mq_buf) + 1, 0) < 0) {
            log_ts("[IPC]", "WARNING: mq_send failed: %s", strerror(errno));
        } else {
            log_ts("[IPC]", "FOUND item #%d successfully enqueued for Matcher process.", new_item.id);
        }
    }

    send_msg(s->sock_fd, "OK Item #%d posted as %s.\n", new_item.id, STATUS_NAMES[type]);
}

/* ================================================================
 * cmd_approve()  – Admin approves a pending match
 * ================================================================ */
static void cmd_approve(Session *s, const char *args) {
    int lost_id = 0, found_id = 0;
    if (sscanf(args, "%d %d", &lost_id, &found_id) != 2) {
        send_msg(s->sock_fd, "ERR Usage: APPROVE <lost_id> <found_id>\n");
        return;
    }

    log_ts("[AUTH]", "Admin '%s' approving match: LOST #%d ↔ FOUND #%d.",
           s->username, lost_id, found_id);

    /* Write-lock items.dat and update both records */
    int fd = open(ITEMS_FILE, O_RDWR);
    if (fd < 0) { send_msg(s->sock_fd, "ERR Cannot open items file.\n"); return; }

    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    log_ts("[LOCK]", "Attempting WRITE lock on %s for APPROVE by Admin '%s'...",
           ITEMS_FILE, s->username);
    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        close(fd); send_msg(s->sock_fd, "ERR Lock failed.\n"); return;
    }
    log_ts("[LOCK]", "WRITE lock ACQUIRED on %s for APPROVE.", ITEMS_FILE);

    int updated = 0;
    Item item;
    off_t offset = 0;
    while (read(fd, &item, sizeof(item)) == sizeof(item)) {
        if (item.active &&
            ((item.status == STATUS_LOST  && item.id == lost_id) ||
             (item.status == STATUS_FOUND && item.id == found_id))) {
            item.status = STATUS_MATCHED;
            lseek(fd, offset, SEEK_SET);
            write(fd, &item, sizeof(item));
            lseek(fd, 0, SEEK_CUR); /* advance */
            updated++;
            log_ts("[RACE-PREVENTION]", "Item #%d status updated to MATCHED under exclusive write lock.", item.id);
        }
        offset += sizeof(item);
    }

    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    log_ts("[LOCK]", "WRITE lock RELEASED on %s after APPROVE.", ITEMS_FILE);
    close(fd);

    /* Remove from pending queue */
    pthread_mutex_lock(&g_shared.pending_mutex);
    log_ts("[MUTEX]", "pending_mutex LOCKED by Admin '%s' for APPROVE cleanup.", s->username);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_shared.pending[i].active &&
            g_shared.pending[i].lost_id  == lost_id &&
            g_shared.pending[i].found_id == found_id) {
            g_shared.pending[i].active = 0;
            g_shared.pending_count--;
            log_ts("[RACE-PREVENTION]", "Pending match slot %d cleared safely under mutex.", i);
            break;
        }
    }
    pthread_mutex_unlock(&g_shared.pending_mutex);
    log_ts("[MUTEX]", "pending_mutex UNLOCKED after APPROVE cleanup.");

    if (updated == 2)
        send_msg(s->sock_fd, "OK Match APPROVED: LOST #%d ↔ FOUND #%d marked as MATCHED.\n", lost_id, found_id);
    else
        send_msg(s->sock_fd, "ERR One or both item IDs not found or status mismatch.\n");
}

/* ================================================================
 * cmd_delete()  – Admin deletes a record
 * ================================================================ */
static void cmd_delete(Session *s, const char *args) {
    int del_id = 0;
    if (sscanf(args, "%d", &del_id) != 1) {
        send_msg(s->sock_fd, "ERR Usage: DELETE <item_id>\n");
        return;
    }

    int fd = open(ITEMS_FILE, O_RDWR);
    if (fd < 0) { send_msg(s->sock_fd, "ERR Cannot open items file.\n"); return; }

    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    log_ts("[LOCK]", "Attempting WRITE lock on %s for DELETE #%d by Admin '%s'...",
           ITEMS_FILE, del_id, s->username);
    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        close(fd); send_msg(s->sock_fd, "ERR Lock failed.\n"); return;
    }
    log_ts("[LOCK]", "WRITE lock ACQUIRED on %s for DELETE.", ITEMS_FILE);

    Item item;
    off_t offset = 0;
    int found = 0;
    while (read(fd, &item, sizeof(item)) == sizeof(item)) {
        if (item.active && item.id == del_id) {
            item.active = 0;
            lseek(fd, offset, SEEK_SET);
            write(fd, &item, sizeof(item));
            found = 1;
            log_ts("[RACE-PREVENTION]", "Item #%d soft-deleted safely under exclusive write lock.", del_id);
            break;
        }
        offset += sizeof(item);
    }

    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    log_ts("[LOCK]", "WRITE lock RELEASED on %s after DELETE.", ITEMS_FILE);
    close(fd);

    if (found)
        send_msg(s->sock_fd, "OK Item #%d deleted.\n", del_id);
    else
        send_msg(s->sock_fd, "ERR Item #%d not found.\n", del_id);
}

/* ================================================================
 * cmd_list_pending()  – show pending matches (mutex-protected)
 * ================================================================ */
static void cmd_list_pending(Session *s) {
    if (s->role < ROLE_ADMIN) {
        log_ts("[AUTH]", "Access DENIED for '%s' (role=%s): LIST_MATCH requires ADMIN.",
               s->username, ROLE_NAMES[s->role]);
        send_msg(s->sock_fd, "DENIED Admin access required.\n");
        return;
    }

    pthread_mutex_lock(&g_shared.pending_mutex);
    log_ts("[MUTEX]", "pending_mutex LOCKED by '%s' for LIST_MATCH.", s->username);

    send_msg(s->sock_fd, "OK Pending matches:\n");
    int printed = 0;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_shared.pending[i].active) {
            send_msg(s->sock_fd, "  Slot %2d:  LOST #%-4d  ↔  FOUND #%d\n",
                     i, g_shared.pending[i].lost_id, g_shared.pending[i].found_id);
            printed++;
        }
    }
    if (printed == 0)
        send_msg(s->sock_fd, "  (No pending matches)\n");
    send_msg(s->sock_fd, "END\n");

    pthread_mutex_unlock(&g_shared.pending_mutex);
    log_ts("[MUTEX]", "pending_mutex UNLOCKED after LIST_MATCH.");
}

/* ================================================================
 * cmd_view_users()  –  Admin: list all registered user accounts
 *
 * OS concepts demonstrated:
 *   • Role-based authorisation check (ADMIN only)
 *   • fcntl() READ lock on users.dat
 *     – Multiple admin sessions may read concurrently (shared lock)
 *     – Any concurrent REGISTER write will block until all readers
 *       have released, preventing a torn read of a half-written record
 *   • Passwords are NEVER sent to the client; only Username and Role
 *     are included in the response (least-privilege information exposure)
 * ================================================================ */
static void cmd_view_users(Session *s) {

    /* ── Role check ─────────────────────────────────────── */
    log_ts("[AUTH]", "Verifying Admin role for VIEW_USERS request from '%s' (%s)...",
           s->username, ROLE_NAMES[s->role]);

    if (s->role < ROLE_ADMIN) {
        log_ts("[AUTH]", "VIEW_USERS DENIED for '%s' (role=%s) – ADMIN required.",
               s->username, ROLE_NAMES[s->role]);
        send_msg(s->sock_fd,
                 "DENIED Only admins can view the user list.\n");
        return;
    }

    log_ts("[AUTH]", "VIEW_USERS authorised for Admin '%s'.", s->username);

    /* ── Open users.dat ─────────────────────────────────── */
    int fd = open(USERS_FILE, O_RDONLY);
    if (fd < 0) {
        log_ts("[LOCK]", "VIEW_USERS: cannot open %s: %s", USERS_FILE, strerror(errno));
        send_msg(s->sock_fd, "ERR Internal error (cannot open users file).\n");
        return;
    }

    /* ── Acquire READ lock ──────────────────────────────── */
    struct flock fl = {
        .l_type   = F_RDLCK,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0    /* lock the entire file */
    };

    log_ts("[LOCK]", "Acquiring READ lock on %s for VIEW_USERS (Admin='%s')...",
           USERS_FILE, s->username);

    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        log_ts("[LOCK]", "READ lock FAILED on %s: %s", USERS_FILE, strerror(errno));
        close(fd);
        send_msg(s->sock_fd, "ERR Lock acquisition failed – try again.\n");
        return;
    }

    log_ts("[LOCK]", "READ lock ACQUIRED on %s for VIEW_USERS.", USERS_FILE);
    log_ts("[RACE-PREVENTION]",
           "Shared read lock held – concurrent REGISTER writes will queue "
           "behind this read to prevent torn records being returned.");

    /* ── Stream records to client ───────────────────────── */
    send_msg(s->sock_fd, "OK\n");
    send_msg(s->sock_fd,
             "  %-4s  %-20s  %-10s  %s\n",
             "No.", "Username", "Role", "Status");
    send_msg(s->sock_fd,
             "  %-4s  %-20s  %-10s  %s\n",
             "----", "--------------------", "----------", "------");

    User u;
    int seq = 0;
    while (read(fd, &u, sizeof(u)) == sizeof(u)) {
        const char *role_label;
        switch (u.role) {
            case ROLE_ADMIN: role_label = "ADMIN";  break;
            case ROLE_USER:  role_label = "USER";   break;
            default:         role_label = "GUEST";  break;
        }
        const char *status_label = u.active ? "Active" : "Deleted";

        send_msg(s->sock_fd,
                 "  %-4d  %-20s  %-10s  %s\n",
                 ++seq, u.username, role_label, status_label);

        log_ts("[AUTH]", "  Sending user record #%d: username='%s' role=%s active=%d",
               seq, u.username, role_label, u.active);
    }

    if (seq == 0)
        send_msg(s->sock_fd, "  (No user records found)\n");

    send_msg(s->sock_fd, "END\n");

    /* ── Release READ lock immediately after reading ─────── */
    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    log_ts("[LOCK]", "READ lock RELEASED on %s after VIEW_USERS (%d record(s) sent).",
           USERS_FILE, seq);

    close(fd);
}

/* ================================================================
 * seed_default_data()  – create files with initial data
 * ================================================================ */
static void seed_default_data(void) {
    /* Users file */
    if (access(USERS_FILE, F_OK) != 0) {
        int fd = open(USERS_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) die("Cannot create users.dat");

        User users[] = {
            { "admin", "admin123", ROLE_ADMIN, 1 },
            { "alice", "pass123",  ROLE_USER,  1 },
            { "bob",   "pass456",  ROLE_USER,  1 },
        };
        for (int i = 0; i < 3; i++)
            write(fd, &users[i], sizeof(User));
        close(fd);
        log_ts("[SERVER]", "Seeded users.dat with 3 default users.");
    }

    /* Items file */
    if (access(ITEMS_FILE, F_OK) != 0) {
        int fd = open(ITEMS_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) die("Cannot create items.dat");

        Item items[] = {
            { 1, STATUS_LOST,  "Black leather wallet",      "Library 2nd floor", "alice", time(NULL) - 86400, 1 },
            { 2, STATUS_LOST,  "Blue umbrella with stars",  "Cafeteria",         "bob",   time(NULL) - 43200, 1 },
            { 3, STATUS_FOUND, "Leather wallet, cards inside", "Library entrance", "bob", time(NULL) - 3600,  1 },
        };
        for (int i = 0; i < 3; i++)
            write(fd, &items[i], sizeof(Item));
        close(fd);
        log_ts("[SERVER]", "Seeded items.dat with 3 sample items.");
    }
}

/* ================================================================
 * I/O helpers
 * ================================================================ */
static void send_msg(int fd, const char *fmt, ...) {
    char buf[BUFFER_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(fd, buf, strlen(buf));
}

static int recv_line(int fd, char *buf, int maxlen) {
    int n = 0;
    char c;
    while (n < maxlen - 1) {
        int r = read(fd, &c, 1);
        if (r <= 0) return r;
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return n;
}

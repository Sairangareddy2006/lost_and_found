/* =========================================================
 * matcher.c  –  Background Matcher Process
 *
 * Responsibilities:
 *   • Opens POSIX MQ to RECEIVE new FOUND items from server
 *   • Reads items.dat (with fcntl read-lock) to scan LOST items
 *   • Performs keyword matching between FOUND and LOST descriptions
 *   • Sends match notifications back to server via result MQ
 *   • Runs as a SEPARATE PROCESS (fork'd by server or launched
 *     independently before/alongside the server)
 *
 * OS Concepts Demonstrated:
 *   • POSIX Message Queues (mq_open, mq_receive, mq_send)
 *   • fcntl() file read locking
 *   • Multi-process architecture (independent process)
 * ========================================================= */

#include "common.h"
#include <stdarg.h>
#include <ctype.h>

/* ── Forward declarations ─────────────────────────────────── */
static int  keyword_match(const char *haystack, const char *needle);
static void lowercase_copy(char *dst, const char *src, int maxlen);
static void scan_for_matches(int found_id, const char *found_desc,
                              const char *found_loc, mqd_t mq_result);

/* ================================================================
 * main()
 * ================================================================ */
int main(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║      LOST & FOUND  –  MATCHER PROCESS  v1.0     ║\n");
    printf("║      PID: %-6d                                 ║\n", getpid());
    printf("╚══════════════════════════════════════════════════╝\n\n");

    log_ts("[IPC]", "Matcher process started (PID=%d). Connecting to message queues...", getpid());

    /* ── Open message queues (server must create them first) ─ */
    /*    Retry loop: wait for server to create the queues     */
    mqd_t mq_found, mq_result;
    int retries = 0;

    while (1) {
        mq_found = mq_open(MQ_NEW_FOUND, O_RDONLY);
        if (mq_found != (mqd_t)-1) break;
        if (++retries > 20) {
            fprintf(stderr, "[MATCHER] ERROR: Could not open '%s' after 20 retries. "
                    "Is the server running?\n", MQ_NEW_FOUND);
            exit(EXIT_FAILURE);
        }
        log_ts("[IPC]", "Waiting for server to create MQ '%s'... (attempt %d)", MQ_NEW_FOUND, retries);
        sleep(1);
    }
    log_ts("[IPC]", "Opened MQ '%s' for reading (new FOUND items).", MQ_NEW_FOUND);

    retries = 0;
    while (1) {
        mq_result = mq_open(MQ_MATCH_RESULT, O_WRONLY);
        if (mq_result != (mqd_t)-1) break;
        if (++retries > 20) {
            fprintf(stderr, "[MATCHER] ERROR: Could not open '%s' after 20 retries.\n", MQ_MATCH_RESULT);
            exit(EXIT_FAILURE);
        }
        log_ts("[IPC]", "Waiting for '%s'... (attempt %d)", MQ_MATCH_RESULT, retries);
        sleep(1);
    }
    log_ts("[IPC]", "Opened MQ '%s' for writing (match results).", MQ_MATCH_RESULT);

    log_ts("[IPC]", "Matcher READY. Waiting for FOUND item notifications from server...\n");

    /* ── Main receive loop ───────────────────────────────── */
    char raw[MQ_MSG_SIZE + 1];
    for (;;) {
        log_ts("[IPC]", "Blocking on mq_receive('%s')...", MQ_NEW_FOUND);

        ssize_t n = mq_receive(mq_found, raw, MQ_MSG_SIZE, NULL);
        if (n < 0) {
            log_ts("[IPC]", "mq_receive error: %s. Retrying in 1s...", strerror(errno));
            sleep(1);
            continue;
        }
        raw[n] = '\0';

        log_ts("[IPC]", "Matcher RECEIVED message from server: '%s'", raw);

        /* Parse: "item_id|description|location" */
        int   found_id   = 0;
        char  found_desc[DESC_LEN] = {0};
        char  found_loc[LOC_LEN]   = {0};
        char  tmp[MQ_MSG_SIZE + 1];
        strncpy(tmp, raw, sizeof(tmp) - 1);

        char *p1 = strchr(tmp, '|');
        if (!p1) {
            log_ts("[IPC]", "Malformed MQ message (no first '|'): '%s'", raw);
            continue;
        }
        *p1 = '\0';
        found_id = atoi(tmp);

        char *p2 = strchr(p1 + 1, '|');
        if (!p2) {
            log_ts("[IPC]", "Malformed MQ message (no second '|'): '%s'", raw);
            continue;
        }
        *p2 = '\0';
        strncpy(found_desc, p1 + 1, DESC_LEN - 1);
        strncpy(found_loc,  p2 + 1, LOC_LEN  - 1);

        log_ts("[IPC]", "Parsed FOUND item: ID=%d, Desc='%s', Loc='%s'",
               found_id, found_desc, found_loc);

        /* Scan for matches */
        scan_for_matches(found_id, found_desc, found_loc, mq_result);
    }

    mq_close(mq_found);
    mq_close(mq_result);
    return 0;
}

/* ================================================================
 * scan_for_matches()
 *   Open items.dat with a READ LOCK, iterate over all LOST items,
 *   check for keyword overlap with the new FOUND item.
 * ================================================================ */
static void scan_for_matches(int found_id, const char *found_desc,
                               const char *found_loc, mqd_t mq_result) {
    log_ts("[LOCK]", "Matcher attempting READ lock on %s for match scan...", ITEMS_FILE);

    int fd = open(ITEMS_FILE, O_RDONLY);
    if (fd < 0) {
        log_ts("[LOCK]", "Cannot open %s: %s", ITEMS_FILE, strerror(errno));
        return;
    }

    struct flock fl = { .l_type = F_RDLCK, .l_whence = SEEK_SET };
    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        log_ts("[LOCK]", "READ lock failed on %s: %s", ITEMS_FILE, strerror(errno));
        close(fd);
        return;
    }
    log_ts("[LOCK]", "READ lock ACQUIRED on %s by Matcher (PID=%d).", ITEMS_FILE, getpid());

    Item item;
    int checked = 0, matches_found = 0;

    log_ts("[IPC]", "Scanning all LOST records for keywords from FOUND #%d...", found_id);

    while (read(fd, &item, sizeof(item)) == sizeof(item)) {
        if (!item.active || item.status != STATUS_LOST)
            continue;
        checked++;

        log_ts("[IPC]", "  Checking LOST #%d ('%s') vs FOUND #%d ('%s')...",
               item.id, item.description, found_id, found_desc);

        /* Check for keyword match */
        char lost_lc[DESC_LEN]  = {0};
        char found_lc[DESC_LEN] = {0};
        char loc_lc[LOC_LEN]    = {0};
        char found_loc_lc[LOC_LEN] = {0};

        lowercase_copy(lost_lc,      item.description, DESC_LEN);
        lowercase_copy(found_lc,     found_desc,        DESC_LEN);
        lowercase_copy(loc_lc,       item.location,     LOC_LEN);
        lowercase_copy(found_loc_lc, found_loc,         LOC_LEN);

        int desc_match = keyword_match(lost_lc, found_lc) ||
                         keyword_match(found_lc, lost_lc);
        int loc_match  = keyword_match(loc_lc, found_loc_lc) ||
                         keyword_match(found_loc_lc, loc_lc);

        if (desc_match || loc_match) {
            log_ts("[IPC]", "  *** POTENTIAL MATCH: LOST #%d ↔ FOUND #%d "
                   "(desc_match=%d, loc_match=%d) ***",
                   item.id, found_id, desc_match, loc_match);

            /* Send result to server via MQ */
            char result_buf[MQ_MSG_SIZE];
            snprintf(result_buf, sizeof(result_buf), "%d:%d", item.id, found_id);

            log_ts("[IPC]", "Matcher sending match result to server: '%s'", result_buf);
            if (mq_send(mq_result, result_buf, strlen(result_buf) + 1, 0) < 0) {
                log_ts("[IPC]", "mq_send(result) ERROR: %s", strerror(errno));
            } else {
                log_ts("[IPC]", "Match notification sent to server MQ successfully.");
                matches_found++;
            }
        } else {
            log_ts("[IPC]", "  No match for LOST #%d.", item.id);
        }
    }

    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    log_ts("[LOCK]", "READ lock RELEASED on %s by Matcher.", ITEMS_FILE);
    close(fd);

    log_ts("[IPC]", "Scan complete for FOUND #%d: checked %d LOST items, found %d potential match(es).",
           found_id, checked, matches_found);
}

/* ================================================================
 * keyword_match()
 *   Simple multi-keyword overlap: splits 'needle' on spaces and
 *   checks if any word of length >= 4 appears in 'haystack'.
 *   Both strings should be lowercase before calling.
 * ================================================================ */
static int keyword_match(const char *haystack, const char *needle) {
    char buf[DESC_LEN];
    strncpy(buf, needle, DESC_LEN - 1);

    char *token = strtok(buf, " ,.-_/\\");
    while (token != NULL) {
        if (strlen(token) >= 4 && strstr(haystack, token) != NULL) {
            log_ts("[IPC]", "    Keyword match found: '%s' appears in target.", token);
            return 1;
        }
        token = strtok(NULL, " ,.-_/\\");
    }
    return 0;
}

/* ── lowercase_copy helper ─────────────────────────────────── */
static void lowercase_copy(char *dst, const char *src, int maxlen) {
    int i;
    for (i = 0; i < maxlen - 1 && src[i]; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

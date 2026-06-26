/* =========================================================
 * client.c  –  Client Process
 *
 * A menu-driven terminal application that connects to the
 * Lost & Found server via TCP and provides a clean UI for
 * all operations based on the user's role.
 * ========================================================= */

#include "common.h"
#include <stdarg.h>

/* ── Terminal colour codes ───────────────────────────────── */
#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_GREEN   "\033[0;32m"
#define COL_YELLOW  "\033[0;33m"
#define COL_CYAN    "\033[0;36m"
#define COL_RED     "\033[0;31m"
#define COL_BLUE    "\033[0;34m"

/* ── State ───────────────────────────────────────────────── */
static int  g_sock = -1;
static char g_username[NAME_LEN]  = "guest";
static char g_role_str[16]        = "GUEST";

/* ── Helpers ─────────────────────────────────────────────── */
static void  send_cmd(const char *fmt, ...);
static void  recv_response(void);
static void  recv_until_end(void);
static char *recv_one_line(char *buf, int maxlen);
static void  clear_screen(void);
static void  print_banner(void);
static void  print_menu(void);
static void  safe_read(const char *prompt, char *buf, int maxlen);

/* ── Menu handlers ───────────────────────────────────────── */
static void do_login(void);
static void do_register(void);
static void do_logout(void);
static void do_list_lost(void);
static void do_list_found(void);
static void do_post_lost(void);
static void do_post_found(void);
static void do_list_matches(void);
static void do_approve_match(void);
static void do_delete_item(void);
static void do_view_users(void);
static void do_disconnect(void);

/* ================================================================
 * main()
 * ================================================================ */
int main(int argc, char *argv[]) {
    char server_ip[64] = "127.0.0.1";
    if (argc >= 2) strncpy(server_ip, argv[1], 63);

    clear_screen();
    print_banner();

    printf(COL_CYAN "  Connecting to server at %s:%d...\n" COL_RESET,
           server_ip, SERVER_PORT);

    /* ── Connect ──────────────────────────────────────────── */
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) die("socket()");

    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port   = htons(SERVER_PORT)
    };
    if (inet_pton(AF_INET, server_ip, &srv.sin_addr) <= 0)
        die("inet_pton()");
    if (connect(g_sock, (struct sockaddr *)&srv, sizeof(srv)) < 0)
        die("connect()");

    printf(COL_GREEN "  ✓ Connected successfully!\n" COL_RESET);

    /* Read the server's welcome message */
    char welcome[BUFFER_SIZE];
    recv_one_line(welcome, sizeof(welcome));
    printf(COL_YELLOW "  Server: %s" COL_RESET, welcome);

    /* Start as guest */
    send_cmd("GUEST");
    char resp[BUFFER_SIZE];
    recv_one_line(resp, sizeof(resp));
    printf(COL_CYAN "  %s" COL_RESET, resp);
    printf("\n");

    /* ── Interactive loop ─────────────────────────────────── */
    char choice[8];
    for (;;) {
        print_menu();
        safe_read("  Enter choice: ", choice, sizeof(choice));

        switch (choice[0]) {
            case '1': do_list_lost();    break;
            case '2': do_list_found();   break;
            case '3':
                if (strcmp(g_role_str, "GUEST") == 0) {
                    printf(COL_RED "  [!] You must log in to post items.\n" COL_RESET);
                } else {
                    do_post_lost();
                }
                break;
            case '4':
                if (strcmp(g_role_str, "GUEST") == 0) {
                    printf(COL_RED "  [!] You must log in to post items.\n" COL_RESET);
                } else {
                    do_post_found();
                }
                break;
            case '5':
                if (strcmp(g_role_str, "ADMIN") != 0) {
                    printf(COL_RED "  [!] Admin access required.\n" COL_RESET);
                } else {
                    do_list_matches();
                }
                break;
            case '6':
                if (strcmp(g_role_str, "ADMIN") != 0) {
                    printf(COL_RED "  [!] Admin access required.\n" COL_RESET);
                } else {
                    do_approve_match();
                }
                break;
            case '7':
                if (strcmp(g_role_str, "ADMIN") != 0) {
                    printf(COL_RED "  [!] Admin access required.\n" COL_RESET);
                } else {
                    do_delete_item();
                }
                break;
            case '8':
                if (strcmp(g_role_str, "ADMIN") != 0) {
                    printf(COL_RED "  [!] Admin access required.\n" COL_RESET);
                } else {
                    do_view_users();
                }
                break;
            case 'L': case 'l':
                if (strcmp(g_role_str, "GUEST") == 0)
                    do_login();
                else
                    do_logout();
                break;
            case 'R': case 'r':
                if (strcmp(g_role_str, "GUEST") != 0) {
                    printf(COL_RED "  [!] You are already logged in. Logout first to register.\n" COL_RESET);
                } else {
                    do_register();
                }
                break;
            case 'H': case 'h':
                send_cmd("HELP");
                recv_until_end();
                break;
            case 'Q': case 'q':
                do_disconnect();
                exit(0);
            default:
                printf(COL_RED "  [!] Invalid choice.\n" COL_RESET);
        }
        printf("\n  Press ENTER to continue...");
        fflush(stdout);
        getchar();
        clear_screen();
    }
    return 0;
}

/* ================================================================
 * Menu printers
 * ================================================================ */
static void print_banner(void) {
    printf(COL_BOLD COL_BLUE);
    printf("  ┌──────────────────────────────────────────────┐\n");
    printf("  │        LOST & FOUND SYSTEM  –  CLIENT        │\n");
    printf("  │          OS Lab Mini-Project  v1.0           │\n");
    printf("  └──────────────────────────────────────────────┘\n");
    printf(COL_RESET "\n");
}

static void print_menu(void) {
    printf(COL_BOLD);
    printf("  ┌──────────────────────────────────────────────┐\n");
    printf("  │  User: %-10s  Role: %-18s  │\n", g_username, g_role_str);
    printf("  ├──────────────────────────────────────────────┤\n");
    printf("  │  [1] List Lost Items                         │\n");
    printf("  │  [2] List Found Items                        │\n");
    printf("  │  [3] Report Lost Item      (User+)           │\n");
    printf("  │  [4] Report Found Item     (User+)           │\n");
    printf("  │  [5] View Pending Matches  (Admin)           │\n");
    printf("  │  [6] Approve a Match       (Admin)           │\n");
    printf("  │  [7] Delete an Item        (Admin)           │\n");
    printf("  │  [8] View All Users        (Admin)           │\n");
    printf("  │  [L] %s                              │\n",
           strcmp(g_role_str, "GUEST") == 0 ? "Login              " : "Logout             ");
    printf("  │  [R] Register New Account  (Guest)           │\n");
    printf("  │  [H] Help                                    │\n");
    printf("  │  [Q] Quit                                    │\n");
    printf("  └──────────────────────────────────────────────┘\n");
    printf(COL_RESET);
}

/* ================================================================
 * Action implementations
 * ================================================================ */
static void do_list_lost(void) {
    printf(COL_CYAN "\n  ── LOST ITEMS ──────────────────────────────────\n" COL_RESET);
    send_cmd("LIST_LOST");
    recv_until_end();
}

static void do_list_found(void) {
    printf(COL_CYAN "\n  ── FOUND ITEMS ─────────────────────────────────\n" COL_RESET);
    send_cmd("LIST_FOUND");
    recv_until_end();
}

static void do_post_lost(void) {
    char desc[DESC_LEN], loc[LOC_LEN];
    printf(COL_CYAN "\n  ── REPORT LOST ITEM ────────────────────────────\n" COL_RESET);
    safe_read("  Description : ", desc, DESC_LEN);
    safe_read("  Location    : ", loc,  LOC_LEN);
    send_cmd("POST_LOST %s|%s", desc, loc);
    recv_response();
}

static void do_post_found(void) {
    char desc[DESC_LEN], loc[LOC_LEN];
    printf(COL_CYAN "\n  ── REPORT FOUND ITEM ───────────────────────────\n" COL_RESET);
    safe_read("  Description : ", desc, DESC_LEN);
    safe_read("  Location    : ", loc,  LOC_LEN);
    send_cmd("POST_FOUND %s|%s", desc, loc);
    recv_response();
    printf(COL_YELLOW
           "  [NOTE] The Matcher process will scan this against LOST items.\n"
           COL_RESET);
}

static void do_list_matches(void) {
    printf(COL_CYAN "\n  ── PENDING MATCHES ─────────────────────────────\n" COL_RESET);
    send_cmd("LIST_MATCH");
    recv_until_end();
}

static void do_approve_match(void) {
    char l_id[16], f_id[16];
    printf(COL_CYAN "\n  ── APPROVE MATCH ───────────────────────────────\n" COL_RESET);
    safe_read("  Lost Item ID  : ", l_id, sizeof(l_id));
    safe_read("  Found Item ID : ", f_id, sizeof(f_id));
    send_cmd("APPROVE %s %s", l_id, f_id);
    recv_response();
}

static void do_delete_item(void) {
    char id[16];
    printf(COL_CYAN "\n  ── DELETE ITEM ─────────────────────────────────\n" COL_RESET);
    safe_read("  Item ID to delete: ", id, sizeof(id));
    send_cmd("DELETE %s", id);
    recv_response();
}

static void do_view_users(void) {
    printf(COL_CYAN "\n  ── ALL REGISTERED USERS ────────────────────────\n" COL_RESET);
    send_cmd("VIEW_USERS");
    recv_until_end();
}

static void do_register(void) {
    char uname[NAME_LEN], pass[PASS_LEN], pass2[PASS_LEN];

    printf(COL_CYAN "\n  ── REGISTER NEW ACCOUNT ────────────────────────\n" COL_RESET);
    printf(COL_YELLOW
           "  New account will be created with role: USER\n"
           "  (Admins can only be created directly on the server)\n\n"
           COL_RESET);

    safe_read("  Choose username (min 3 chars, no spaces) : ", uname, NAME_LEN);
    if (strlen(uname) < 3) {
        printf(COL_RED "  [!] Username too short.\n" COL_RESET);
        return;
    }

    safe_read("  Choose password (min 4 chars)            : ", pass,  PASS_LEN);
    if (strlen(pass) < 4) {
        printf(COL_RED "  [!] Password too short.\n" COL_RESET);
        return;
    }

    safe_read("  Confirm password                         : ", pass2, PASS_LEN);
    if (strcmp(pass, pass2) != 0) {
        printf(COL_RED "  [!] Passwords do not match. Registration cancelled.\n" COL_RESET);
        return;
    }

    /* Send REGISTER command to server */
    send_cmd("REGISTER %s %s", uname, pass);

    /* Read and display the server's single-line response */
    char resp[BUFFER_SIZE];
    recv_one_line(resp, sizeof(resp));
    resp[strcspn(resp, "\r\n")] = '\0';

    if (strncmp(resp, "OK", 2) == 0) {
        printf(COL_GREEN "  ✓ %s\n" COL_RESET, resp + 3);
        printf(COL_YELLOW "  Tip: Press [L] to log in with your new account.\n" COL_RESET);
    } else if (strncmp(resp, "ERR", 3) == 0) {
        printf(COL_RED "  ✗ Server error: %s\n" COL_RESET, resp + 4);
    } else {
        printf(COL_RED "  ✗ Unexpected response: %s\n" COL_RESET, resp);
    }
}

static void do_login(void) {
    char uname[NAME_LEN], pass[PASS_LEN];
    printf(COL_CYAN "\n  ── LOGIN ───────────────────────────────────────\n" COL_RESET);
    safe_read("  Username : ", uname, NAME_LEN);
    safe_read("  Password : ", pass,  PASS_LEN);
    send_cmd("LOGIN %s %s", uname, pass);

    char resp[BUFFER_SIZE];
    recv_one_line(resp, sizeof(resp));
    resp[strcspn(resp, "\r\n")] = '\0';

    if (strncmp(resp, "OK", 2) == 0) {
        strncpy(g_username, uname, NAME_LEN - 1);
        /* Parse role from "OK Welcome, alice! Role: USER" */
        char *role_ptr = strstr(resp, "Role: ");
        if (role_ptr) {
            strncpy(g_role_str, role_ptr + 6, 15);
            g_role_str[strcspn(g_role_str, " \t\r\n")] = '\0';
        }
        printf(COL_GREEN "  ✓ %s\n" COL_RESET, resp + 3);
    } else {
        printf(COL_RED "  ✗ %s\n" COL_RESET, resp + 4);
    }
}

static void do_logout(void) {
    send_cmd("LOGOUT");
    recv_response();
    strncpy(g_username, "guest", NAME_LEN - 1);
    strncpy(g_role_str, "GUEST", 15);
    /* Switch back to guest mode */
    send_cmd("GUEST");
    char resp[BUFFER_SIZE];
    recv_one_line(resp, sizeof(resp));
}

static void do_disconnect(void) {
    send_cmd("QUIT");
    char buf[BUFFER_SIZE];
    recv_one_line(buf, sizeof(buf));
    printf(COL_YELLOW "\n  %s" COL_RESET, buf);
    close(g_sock);
    printf(COL_GREEN "  Connection closed. Goodbye!\n\n" COL_RESET);
}

/* ================================================================
 * I/O helpers
 * ================================================================ */
static void send_cmd(const char *fmt, ...) {
    char buf[BUFFER_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    buf[n]   = '\n';
    buf[n+1] = '\0';
    write(g_sock, buf, n + 1);
}

static void recv_response(void) {
    char buf[BUFFER_SIZE];
    recv_one_line(buf, sizeof(buf));
    buf[strcspn(buf, "\r\n")] = '\0';
    if (strncmp(buf, "OK", 2) == 0)
        printf(COL_GREEN "  ✓ %s\n" COL_RESET, buf + 3);
    else if (strncmp(buf, "DENIED", 6) == 0)
        printf(COL_RED "  ✗ ACCESS DENIED: %s\n" COL_RESET, buf + 7);
    else
        printf(COL_RED "  ✗ %s\n" COL_RESET, buf + 4);
}

/* Read lines until "END\n" or connection closed */
static void recv_until_end(void) {
    char buf[BUFFER_SIZE];
    for (;;) {
        if (!recv_one_line(buf, sizeof(buf))) break;
        if (strcmp(buf, "END\n") == 0 || strcmp(buf, "END\r\n") == 0) break;
        /* Skip leading "OK\n" */
        if (strcmp(buf, "OK\n") == 0) continue;
        printf("  %s", buf);
    }
}

static char *recv_one_line(char *buf, int maxlen) {
    int n = 0;
    char c;
    while (n < maxlen - 1) {
        int r = read(g_sock, &c, 1);
        if (r <= 0) { buf[n] = '\0'; return n > 0 ? buf : NULL; }
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return buf;
}

static void safe_read(const char *prompt, char *buf, int maxlen) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, maxlen, stdin)) buf[0] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
}

static void clear_screen(void) {
    printf("\033[2J\033[H");
}

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "sqlite3.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

#define TORE_FILENAME ".tore"

#define LOG_SQLITE3_ERROR(db) fprintf(stderr, "%s:%d: SQLITE3 ERROR: %s\n", __FILE__, __LINE__, sqlite3_errmsg(db))

const char *migrations[] = {
    // Initial scheme
    "CREATE TABLE IF NOT EXISTS Notifications (\n"
    "    id INTEGER PRIMARY KEY ASC,\n"
    "    title TEXT NOT NULL,\n"
    "    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
    "    dismissed_at DATETIME DEFAULT NULL\n"
    ");\n",
    "CREATE TABLE IF NOT EXISTS Reminders (\n"
    "    id INTEGER PRIMARY KEY ASC,\n"
    "    title TEXT NOT NULL,\n"
    "    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
    "    scheduled_at DATE NOT NULL,\n"
    "    period TEXT DEFAULT NULL,\n"
    "    finished_at DATETIME DEFAULT NULL\n"
    ");\n",

    // Add reference to the Reminder that created the Notification
    "ALTER TABLE Notifications RENAME TO Notifications_old;\n"
    "CREATE TABLE IF NOT EXISTS Notifications (\n"
    "    id INTEGER PRIMARY KEY ASC,\n"
    "    title TEXT NOT NULL,\n"
    "    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
    "    dismissed_at DATETIME DEFAULT NULL,\n"
    "    reminder_id INTEGER DEFAULT NULL,\n"
    "    FOREIGN KEY (reminder_id) REFERENCES Reminders(id)\n"
    ");\n"
    "INSERT INTO Notifications (id, title, created_at, dismissed_at)\n"
    "SELECT id, title, created_at, dismissed_at FROM Notifications_old;\n"
    "DROP TABLE Notifications_old;\n"
};

// TODO: can we just extract tore_path from db somehow?
bool create_schema(sqlite3 *db, const char *tore_path)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "CREATE TABLE IF NOT EXISTS Migrations (\n"
        "    applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
        "    query TEXT NOT NULL\n"
        ");\n";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    if (sqlite3_prepare_v2(db, "SELECT query FROM Migrations;", -1, &stmt, NULL)!= SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    size_t index = 0;
    int ret = sqlite3_step(stmt);
    for (; ret == SQLITE_ROW; ++index) {
        if (index >= ARRAY_LEN(migrations)) {
            fprintf(stderr, "ERROR: %s: Database scheme is too new. Contains more migrations applied than expected. Update your application.\n", tore_path);
            return_defer(false);
        }
        const char *query = (const char *)sqlite3_column_text(stmt, 0);
        if (strcmp(query, migrations[index]) != 0) {
            fprintf(stderr, "ERROR: %s: Invalid database scheme. Mismatch in migration %zu:\n", tore_path, index);
            fprintf(stderr, "EXPECTED: %s\n", migrations[index]);
            fprintf(stderr, "FOUND: %s\n", query);
            return_defer(false);
        }
        ret = sqlite3_step(stmt);
    }

    if (ret != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    bool tore_trace_migration_queries = getenv("TORE_TRACE_MIGRATION_QUERIES") != NULL;
    for (; index < ARRAY_LEN(migrations); ++index) {
        printf("INFO: %s: applying migration %zu\n", tore_path, index);
        if (tore_trace_migration_queries) printf("%s\n", migrations[index]);
        if (sqlite3_exec(db, migrations[index], NULL, NULL, NULL) != SQLITE_OK) {
            LOG_SQLITE3_ERROR(db);
            return_defer(false);
        }

        int ret = sqlite3_prepare_v2(db, "INSERT INTO Migrations (query) VALUES (?)", -1, &stmt, NULL);
        if (ret != SQLITE_OK) {
            LOG_SQLITE3_ERROR(db);
            return_defer(false);
        }

        if (sqlite3_bind_text(stmt, 1, migrations[index], strlen(migrations[index]), NULL) != SQLITE_OK) {
            LOG_SQLITE3_ERROR(db);
            return_defer(false);
        }

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            LOG_SQLITE3_ERROR(db);
            return_defer(false);
        }

        sqlite3_finalize(stmt);
        stmt = NULL;
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

typedef struct {
    int id;
    const char *title;
    const char *created_at;
    int reminder_id;
    // NOTE: count > 1 means that reminder_id >= 0 and in the database there are several active notifications created by the same Reminder.
    // So this means all those (basically the same) Notifications got collapsed into a single one for convenient displaying.
    // id refers to whatever Notification Sqlite3 decides after the GROUP BY (usually the first one).
    int count;
} Collapsed_Notification;

typedef struct {
    Collapsed_Notification *items;
    size_t count;
    size_t capacity;
} Collapsed_Notifications;

bool load_active_collapsed_notifications(sqlite3 *db, Collapsed_Notifications *notifs)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;

    int ret = sqlite3_prepare_v2(db,
        "SELECT id, title, datetime(created_at, 'localtime') as ts, reminder_id, count(*) "
        "FROM Notifications "
        "WHERE dismissed_at IS NULL "
        "GROUP BY ifnull(reminder_id, -id) "
        "ORDER BY ts;",
        -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    ret = sqlite3_step(stmt);
    for (int row = 0; ret == SQLITE_ROW; ++row) {
        int column = 0;
        int id = sqlite3_column_int(stmt, column++);
        // TODO: maybe put all of these dupped strings into their own arena
        const char *title = strdup((const char *)sqlite3_column_text(stmt, column++));
        const char *created_at = strdup((const char *)sqlite3_column_text(stmt, column++));
        int reminder_id = sqlite3_column_int(stmt, column++);
        int count = sqlite3_column_int(stmt, column++);
        da_append(notifs, ((Collapsed_Notification) {
            .id = id,
            .title = title,
            .created_at = created_at,
            .reminder_id = reminder_id,
            .count = count,
        }));
        ret = sqlite3_step(stmt);
    }

    if (ret != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

bool show_active_notifications(sqlite3 *db)
{
    bool result = true;

    Collapsed_Notifications notifs = {0};
    if (!load_active_collapsed_notifications(db, &notifs)) return_defer(false);

    // TODO: Consider using UUIDs for identifying Notifications and Reminders
    //   Read something like https://www.cockroachlabs.com/blog/what-is-a-uuid/ for UUIDs in DBs 101
    //   (There are lots of articles like these online, just google the topic up).
    //   This is related to visually grouping non-dismissed Notifications created by the same Reminders purely in SQL.
    //   Doing it straightforwardly would be something like
    //   ```sql
    //   SELECT id, title, datetime(created_at, 'localtime') FROM Notifications WHERE dismissed_at IS NULL GROUP BY ifnull(reminder_id, id)
    //   ```
    //   but you may run into problems if reminder_id and id collide. Using UUIDs for all the rows of all the tables solves this.
    //   Right now it is solved by making the row id negative.
    //   ```sql
    //   SELECT id, title, datetime(created_at, 'localtime') FROM Notifications WHERE dismissed_at IS NULL GROUP BY ifnull(reminder_id, -id)
    //   ```
    //   Which is a working solution, but all the other problems UUIDs address remain.

    for (size_t i = 0; i < notifs.count; ++i) {
        Collapsed_Notification *it = &notifs.items[i];
        assert(it->count >= 0);
        if (it->count == 1) {
            printf("%zu: %s (%s)\n", i, it->title, it->created_at);
        } else {
            printf("%zu: [%d] %s (%s)\n", i, it->count, it->title, it->created_at);
        }
    }

defer:
    free(notifs.items);
    return result;
}

bool dismiss_notification_by_id(sqlite3 *db, int id)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;

    int ret = sqlite3_prepare_v2(db, "UPDATE Notifications SET dismissed_at = CURRENT_TIMESTAMP WHERE id = ?", -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    if (sqlite3_bind_int(stmt, 1, id) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

bool dismiss_notification_by_index(sqlite3 *db, int index)
{
    bool result = true;

    Collapsed_Notifications notifs = {0};
    if (!load_active_collapsed_notifications(db, &notifs)) return_defer(false);
    if (!(0 <= index && (size_t)index < notifs.count)) {
        fprintf(stderr, "ERROR: %d is not a valid index of an active notification\n", index);
        return_defer(false);
    }
    if (!dismiss_notification_by_id(db, notifs.items[index].id)) return_defer(false);

defer:
    free(notifs.items);
    return result;
}

bool create_notification_with_title(sqlite3 *db, const char *title)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, "INSERT INTO Notifications (title) VALUES (?)", -1, &stmt, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    if (sqlite3_bind_text(stmt, 1, title, strlen(title), NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

typedef struct {
    int id;
    const char *title;
    const char *scheduled_at;
    const char *period;
} Reminder;

typedef struct {
    Reminder *items;
    size_t count;
    size_t capacity;
} Reminders;

bool load_active_reminders(sqlite3 *db, Reminders *reminders)
{
    bool result = true;

    sqlite3_stmt *stmt = NULL;

    int ret = sqlite3_prepare_v2(db, "SELECT id, title, scheduled_at, period FROM Reminders WHERE finished_at IS NULL ORDER BY scheduled_at DESC", -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    ret = sqlite3_step(stmt);
    for (int index = 0; ret == SQLITE_ROW; ++index) {
        int id = sqlite3_column_int(stmt, 0);
        const char *title = strdup((const char *)sqlite3_column_text(stmt, 1));
        const char *scheduled_at = strdup((const char *)sqlite3_column_text(stmt, 2));
        const char *period = (const char *)sqlite3_column_text(stmt, 3);
        if (period != NULL) period = strdup(period);
        da_append(reminders, ((Reminder) {
            .id = id,
            .title = title,
            .scheduled_at = scheduled_at,
            .period = period,
        }));
        ret = sqlite3_step(stmt);
    }

    if (ret != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
defer:
    return result;
}

typedef enum {
    PERIOD_NONE = -1,
    PERIOD_DAY,
    PERIOD_WEEK,
    PERIOD_MONTH,
    PERIOD_YEAR,
    COUNT_PERIODS,
} Period;

typedef struct {
    const char *modifier;
    const char *name;
} Period_Modifier;

static_assert(COUNT_PERIODS == 4, "Amount of periods have changed");
Period_Modifier tore_period_modifiers[COUNT_PERIODS] = {
    [PERIOD_DAY]   = { .modifier = "d", .name = "days"   },
    [PERIOD_WEEK]  = { .modifier = "w", .name = "weeks"  },
    [PERIOD_MONTH] = { .modifier = "m", .name = "months" },
    [PERIOD_YEAR]  = { .modifier = "y", .name = "years"  },
};

Period period_by_tore_modifier(const char *modifier)
{
    for (Period period = PERIOD_NONE; period < COUNT_PERIODS; ++period) {
        if (strcmp(modifier, tore_period_modifiers[period].modifier) == 0) {
            return period;
        }
    }
    return PERIOD_NONE;
}

const char *render_period_as_sqlite3_datetime_modifier_temp(Period period, unsigned long period_length)
{
    switch (period) {
    case PERIOD_NONE:  return NULL;
    case PERIOD_DAY:   return temp_sprintf("+%lu days",   period_length);
    case PERIOD_WEEK:  return temp_sprintf("+%lu days",   period_length*7);
    case PERIOD_MONTH: return temp_sprintf("+%lu months", period_length);
    case PERIOD_YEAR:  return temp_sprintf("+%lu years",  period_length);
    case COUNT_PERIODS:
    default: UNREACHABLE("render_period_as_sqlite3_datetime_modifier_temp");
    }
}

bool create_new_reminder(sqlite3 *db, const char *title, const char *scheduled_at, Period period, unsigned long period_length)
{
    bool result = true;

    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, "INSERT INTO Reminders (title, scheduled_at, period) VALUES (?, ?, ?)", -1, &stmt, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    if (sqlite3_bind_text(stmt, 1, title, strlen(title), NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    if (sqlite3_bind_text(stmt, 2, scheduled_at, strlen(scheduled_at), NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    const char *rendered_period = render_period_as_sqlite3_datetime_modifier_temp(period, period_length);
    if (sqlite3_bind_text(stmt, 3, rendered_period, rendered_period ? strlen(rendered_period) : 0, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

// NOTE: The general policy of the application is that all the date times are stored in GMT, but before displaying them and/or making logical decisions upon them they are converted to localtime.
bool fire_off_reminders(sqlite3 *db)
{
    bool result = true;

    sqlite3_stmt *stmt = NULL;

    // Creating new notifications from fired off reminders
    const char *sql = "INSERT INTO Notifications (title, reminder_id) SELECT title, id FROM Reminders WHERE scheduled_at <= date('now', 'localtime') AND finished_at IS NULL";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    // Finish all the non-periodic reminders
    sql = "UPDATE Reminders SET finished_at = CURRENT_TIMESTAMP WHERE scheduled_at <= date('now', 'localtime') AND finished_at IS NULL AND period is NULL";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    // Reschedule all the period reminders
    sql = "UPDATE Reminders SET scheduled_at = date(scheduled_at, period) WHERE scheduled_at <= date('now', 'localtime') AND finished_at IS NULL AND period is NOT NULL";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    sqlite3_finalize(stmt);
    return result;
}

bool show_active_reminders(sqlite3 *db)
{
    bool result = true;

    Reminders reminders = {0};

    // TODO: show in how many days the reminder fires off
    if (!load_active_reminders(db, &reminders)) return_defer(false);
    for (size_t i = 0; i < reminders.count; ++i) {
        Reminder *it = &reminders.items[i];
        if (it->period) {
            fprintf(stderr, "%zu: %s (Scheduled at %s every %s)\n", i, it->title, it->scheduled_at, it->period);
        } else {
            fprintf(stderr, "%zu: %s (Scheduled at %s)\n", i, it->title, it->scheduled_at);
        }
    }

defer:
    free(reminders.items);
    return result;
}

bool remove_reminder_by_id(sqlite3 *db, int id)
{
    bool result = true;

    sqlite3_stmt *stmt = NULL;

    int ret = sqlite3_prepare_v2(db, "UPDATE Reminders SET finished_at = CURRENT_TIMESTAMP WHERE id = ?", -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    if (sqlite3_bind_int(stmt, 1, id) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

bool remove_reminder_by_number(sqlite3 *db, int number)
{
    bool result = true;

    Reminders reminders = {0};
    if (!load_active_reminders(db, &reminders)) return_defer(false);
    if (!(0 <= number && (size_t)number < reminders.count)) {
        fprintf(stderr, "ERROR: %d is not a valid index of a reminder\n", number);
        return_defer(false);
    }
    if (!remove_reminder_by_id(db, reminders.items[number].id)) return_defer(false);

defer:
    free(reminders.items);
    return result;
}

bool verify_date_format(const char *date)
{
    // Who needs Regular Expressions?
    const char *format = "dddd-dd-dd";
    for (; *format && *date; format++, date++) {
        switch (*format) {
            case 'd': if (!isdigit(*date)) return false; break;
            case '-': if (*date != '-')    return false; break;
            default:  UNREACHABLE("verify_date_format");
        }
    }
    return !(*format || *date);
}

// Taken from https://stackoverflow.com/a/7382028
void sb_append_html_escaped_buf(String_Builder *sb, const char *buf, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        switch (buf[i]) {
            case '&':  sb_append_cstr(sb, "&amp;");  break;
            case '<':  sb_append_cstr(sb, "&lt;");   break;
            case '>':  sb_append_cstr(sb, "&gt;");   break;
            case '"':  sb_append_cstr(sb, "&quot;"); break;
            case '\'': sb_append_cstr(sb, "&#39;");  break;
            default:   da_append(sb, buf[i]);
        }
    }
}

void render_index_page(String_Builder *sb, Collapsed_Notifications notifs, Reminders reminders)
{
#define OUT(buf, size) sb_append_buf(sb, buf, size)
#define ESCAPED_OUT(buf, size) sb_append_html_escaped_buf(sb, buf, size)
#define INT(x) sb_append_cstr(sb, temp_sprintf("%d", (x)))
#include "index_page.h"
#undef INT
#undef OUT
#undef ESCAPED_OUT
}

void print_prog_info() {
    printf("Tore (Total REcall)\nhttps://github.com/rexim/tore\n");
    printf("Version: "GIT_HASH"\n");
}

void print_usages(const char *program_name) {
    printf("Usage: %s [command] [options]\n\n", program_name);
    printf("help         Display this help message\n");
    printf("\u21AA Usage: %s help\n", program_name);
    printf("version      Display the latest git hash\n");
    printf("\u21AA Usage: %s version\n", program_name);
    printf("serve        Serve a webpage displaying notifications/reminders at http://<addr>:<port> (Default: http://127.0.0.1:6969)\n");
    printf("\u21AA Usage: %s serve [--addr=<address> | --addr <address>] [--port=<port> | --port <port>]\n", program_name);

    printf("\nNotifications:\n");
    printf("notify       Create a notification\n");
    printf("\u21AA Usage: %s notify <title...>\n", program_name);
    printf("dismiss      Dismiss an active notification by index\n");
    printf("\u21AA Usage: %s dismiss <index>\n", program_name);

    printf("\nReminders:\n");
    printf("remind       Create a reminder\n");
    printf("\u21AA Usage: %s remind [<title> <scheduled_at> [period]]\n", program_name);
    printf("checkout     Consume then display active reminders\n");
    printf("\u21AA Usage: %s checkout\n", program_name);
    printf("forget       Forget (delete) a reminder by index\n");
    printf("\u21AA Usage: %s forget <index>\n", program_name);
}


int main(int argc, char **argv)
{
    int result = 0;
    sqlite3 *db = NULL;
    String_Builder sb = {0};

    srand(time(0));

    const char *program_name = shift(argv, argc);

    const char *command_name= "checkout";
    if (argc > 0) command_name = shift(argv, argc);

    if (strcmp(command_name, "help") == 0) {
        print_prog_info();
        print_usages(program_name);
        return 0;
    }

    if (strcmp(command_name, "version") == 0) {
        fprintf(stderr, "GIT HASH: "GIT_HASH"\n");
        return 0;
    }

    const char *home_path = getenv("HOME");
    if (home_path == NULL) {
        fprintf(stderr, "ERROR: No $HOME environment variable is setup. We need it to find the location of ~/"TORE_FILENAME" database.\n");
        return_defer(1);
    }

    const char *tore_path = temp_sprintf("%s/"TORE_FILENAME, home_path);

    int ret = sqlite3_open(tore_path, &db);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "ERROR: %s: %s\n", tore_path, sqlite3_errstr(ret));
        return_defer(1);
    }

    if (!create_schema(db, tore_path)) return_defer(1);

    // TODO: `undo` command

    if (strcmp(command_name, "checkout") == 0) {
        if (!fire_off_reminders(db)) return_defer(1);
        if (!show_active_notifications(db)) return_defer(1);
        // TODO: show reminders that are about to fire off
        //   Maybe they should fire off a "warning" notification before doing the main one?
        return_defer(0);
    }

    // TODO: maybe `dismiss` should dismiss the entire group of Collapsed Notifications?
    // TODO: `dismiss` should accept several indices
    if (strcmp(command_name, "dismiss") == 0) {
        if (argc <= 0) {
            fprintf(stderr, "Usage: %s dismiss <index>\n", program_name);
            fprintf(stderr, "ERROR: expected index\n");
            return_defer(1);
        }

        int index = atoi(shift(argv, argc));
        if (!dismiss_notification_by_index(db, index)) return_defer(1);
        if (!show_active_notifications(db)) return_defer(1);
        return_defer(0);
    }

    if (strcmp(command_name, "serve") == 0) {
        const char *usage = "Usage: %s serve [--addr=<address> | --addr <address>] [--port=<port> | --port <port>]\n";
        const char *addr = "127.0.0.1";
        uint16_t port = 6969;

        while (argc > 0) {
            const char *arg = shift(argv, argc);
            char *value = strchr(arg, '=');
            if (value) *(value++) = '\0';
            else if (argc > 0) value = shift(argv, argc);
            else {
                fprintf(stderr, usage, program_name);
                fprintf(stderr, "ERROR: invalid argument format\n");
                return_defer(1);
            }

            if (strcmp(arg, "--addr") == 0) addr = value;
            else if (strcmp(arg, "--port") == 0) port = (uint16_t)atoi(value);
            else {
                fprintf(stderr, usage, program_name);
                fprintf(stderr, "ERROR: unexpected argument provided\n");
                return_defer(1);
            }
        }

        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            fprintf(stderr, "ERROR: Could not create socket epicly: %s\n", strerror(errno));
            return_defer(1);
        }

        int option = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = inet_addr(addr);

        ssize_t err = bind(server_fd, (struct sockaddr*) &server_addr, sizeof(server_addr));
        if (err != 0) {
            fprintf(stderr, "ERROR: Could not bind socket epicly: %s\n", strerror(errno));
            return_defer(1);
        }

        err = listen(server_fd, 69);
        if (err != 0) {
            fprintf(stderr, "ERRO: Could not listen to socket, it's too quiet: %s\n", strerror(errno));
            return_defer(1);
        }

        printf("Listening to http://%s:%d/\n", addr, port);

        Collapsed_Notifications notifs = {0};
        Reminders reminders = {0};
        String_Builder response = {0};
        String_Builder body = {0};
        for (;;) {
            // TODO: log queries
            struct sockaddr_in client_addr;
            socklen_t client_addrlen = 0;
            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addrlen);
            if (client_fd < 0) {
                fprintf(stderr, "ERROR: Could not accept connection. This is unacceptable! %s\n", strerror(errno));
                return_defer(1);
            }

            if (!load_active_collapsed_notifications(db, &notifs)) return_defer(false);
            if (!load_active_reminders(db, &reminders)) return_defer(false);
            render_index_page(&body, notifs, reminders);

            sb_append_cstr(&response, "HTTP/1.0 200\r\n");
            sb_append_cstr(&response, "Content-Type: text/html\r\n");
            sb_append_cstr(&response, temp_sprintf("Content-Length: %zu\r\n", body.count));
            sb_append_cstr(&response, "Connection: close\r\n");
            sb_append_cstr(&response, "\r\n");
            sb_append_buf(&response, body.items, body.count);

            String_View untransfered = sb_to_sv(response);
            while (untransfered.count > 0) {
                ssize_t transfered = write(client_fd, untransfered.data, untransfered.count);
                if (transfered < 0) {
                    fprintf(stderr, "ERROR: Could not write response: %s\n", strerror(errno));
                    break;
                }
                untransfered.data += transfered;
                untransfered.count -= transfered;
            }

            close(client_fd);

            notifs.count = 0;
            reminders.count = 0;
            body.count = 0;
            response.count = 0;
            temp_reset();
        }

        // TODO: The only way to stop the server is by SIGINT, but that probably doesn't close the db correctly.
        // So we probably should add a SIGINT handler specifically for this.

        UNREACHABLE("serve");
    }

    if (strcmp(command_name, "notify") == 0) {
        if (argc <= 0) {
            fprintf(stderr, "Usage: %s notify <title...>\n", program_name);
            fprintf(stderr, "ERROR: expected title\n");
            return_defer(1);
        }

        for (bool pad = false; argc > 0; pad = true) {
            if (pad) sb_append_cstr(&sb, " ");
            sb_append_cstr(&sb, shift(argv, argc));
        }
        sb_append_null(&sb);
        const char *title = sb.items;

        if (!create_notification_with_title(db, title)) return_defer(1);
        if (!show_active_notifications(db)) return_defer(1);
        return_defer(0);
    }

    if (strcmp(command_name, "forget") == 0) {
        if (argc <= 0) {
            fprintf(stderr, "Usage: %s forget <index>\n", program_name);
            fprintf(stderr, "ERROR: expected index\n");
            return_defer(1);
        }
        int number = atoi(shift(argv, argc));
        if (!remove_reminder_by_number(db, number)) return_defer(1);
        if (!show_active_reminders(db)) return_defer(1);
        return_defer(0);
    }

    if (strcmp(command_name, "remind") == 0) {
        if (argc <= 0) {
            if (!show_active_reminders(db)) return_defer(1);
            return_defer(0);
        }

        const char *title = shift(argv, argc);
        if (argc <= 0) {
            fprintf(stderr, "Usage: %s remind [<title> <scheduled_at> [period]]\n", program_name);
            fprintf(stderr, "ERROR: expected scheduled_at\n");
            return_defer(1);
        }

        // TODO: Allow the scheduled_at to be things like "today", "tomorrow", etc
        // TODO: research if it's possible to enforce the date format on the level of sqlite3 contraints
        const char *scheduled_at = shift(argv, argc);
        if (!verify_date_format(scheduled_at)) {
            fprintf(stderr, "ERROR: %s is not a valid date format\n", scheduled_at);
            return_defer(1);
        }

        Period period = PERIOD_NONE;
        unsigned long period_length = 0;
        if (argc > 0) {
            const char *unparsed_period = shift(argv, argc);
            char *endptr = NULL;
            period_length = strtoul(unparsed_period, &endptr, 10);
            if (endptr == unparsed_period) {
                fprintf(stderr, "ERROR: Invalid period `%s`. Expected something like\n", unparsed_period);
                for (Period p = 0; p < COUNT_PERIODS; ++p) {
                    Period_Modifier *pm = &tore_period_modifiers[p];
                    size_t l = rand()%9 + 1;
                    fprintf(stderr, "    %lu%s - means every %lu %s\n", l, pm->modifier, l, pm->name);
                }
                return_defer(1);
            }
            unparsed_period = endptr;
            period = period_by_tore_modifier(unparsed_period);
            if (period == PERIOD_NONE) {
                fprintf(stderr, "ERROR: Unknown period modifier `%s`. Expected modifiers are\n", unparsed_period);
                for (Period p = 0; p < COUNT_PERIODS; ++p) {
                    Period_Modifier *pm = &tore_period_modifiers[p];
                    fprintf(stderr, "    %lu%s  - means every %lu %s\n", period_length, pm->modifier, period_length, pm->name);
                }
                return_defer(1);
            }
        }

        if (!create_new_reminder(db, title, scheduled_at, period, period_length)) return_defer(1);
        if (!show_active_reminders(db)) return_defer(1);
        return_defer(0);
    }

    // TODO: some way to turn Notification into a Reminder

    fprintf(stderr, "ERROR: unknown command %s\n", command_name);
    print_usages(program_name);
    return_defer(1);

defer:
    if (db) sqlite3_close(db);
    free(sb.items);
    return result;
}

// TODO: start using Sqlite3 Transactions
// - Wrap each command into a transaction
// - Wrap each `serve` request into a transaction
// TODO: calendar output with the reminders

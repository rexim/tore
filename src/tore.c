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

#include "bundle.h"

#define TORE_FILENAME ".tore"
#define STR(x) STR2_ELECTRIC_BOOGALOO(x)
#define STR2_ELECTRIC_BOOGALOO(x) #x
#define DEFAULT_SERVE_PORT 6969
#define DEFAULT_COMMAND "checkout"

#define LOG_SQLITE3_ERROR(db) fprintf(stderr, "%s:%d: SQLITE3 ERROR: %s\n", __FILE__, __LINE__, sqlite3_errmsg(db))

bool txn_begin(sqlite3 *db)
{
    if (sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return false;
    }
    return true;
}

bool txn_commit(sqlite3 *db)
{
    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return false;
    }
    return true;
}

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
    if (!txn_begin(db)) return_defer(false);
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
    if (result) result = txn_commit(db);
    return result;
}

typedef struct {
    int id;
    const char *title;
    const char *created_at;
    const char *dismissed_at;
    int reminder_id;
    int group_id;    // something that uniquely identifies a group of notifications and it is computed as ifnull(reminder_id, -id)
} Notification;

typedef struct {
    Notification *items;
    size_t count;
    size_t capacity;
} Notifications;

int load_notification_by_id(sqlite3 *db, int notif_id, Notification *notif)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;

    int ret = sqlite3_prepare_v2(db,
        "SELECT\n"
        "    id,\n"
        "    title,\n"
        "    datetime(created_at, 'localtime'),\n"
        "    datetime(dismissed_at, 'localtime'),\n"
        "    reminder_id,\n"
        "    ifnull(reminder_id, -id)\n"
        "FROM Notifications WHERE id = ?;",
        -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(-1);
    }

    if (sqlite3_bind_int(stmt, 1, notif_id) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(-1);
    }

    ret = sqlite3_step(stmt);
    if (ret == SQLITE_DONE) {
        // nothing found
        return_defer(0);
    }

    if (ret != SQLITE_ROW) {
        LOG_SQLITE3_ERROR(db);
        return_defer(-1);
    }

    int column = 0;
    int id = sqlite3_column_int(stmt, column++);
    const char *title = (const char *)sqlite3_column_text(stmt, column++);
    const char *created_at = (const char *)sqlite3_column_text(stmt, column++);
    const char *dismissed_at = (const char *)sqlite3_column_text(stmt, column++);
    int reminder_id = sqlite3_column_int(stmt, column++);
    int group_id = sqlite3_column_int(stmt, column++);

    *notif = (Notification) {
        .id           = id,
        .title        = title        ? temp_strdup(title)        : NULL,
        .created_at   = created_at   ? temp_strdup(created_at)   : NULL,
        .dismissed_at = dismissed_at ? temp_strdup(dismissed_at) : NULL,
        .reminder_id  = reminder_id,
        .group_id     = group_id,
    };

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

bool load_active_notifications_of_group(sqlite3 *db, int group_id, Notifications *ns)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;

    int ret = sqlite3_prepare_v2(db,
        "SELECT\n"
        "    id,\n"
        "    title,\n"
        "    datetime(created_at, 'localtime') as ts,\n"
        "    datetime(dimissed_at, 'localtime'),\n"
        "    reminder_id,\n"
        "    ifnull(reminder_id, -id) as group_id\n"
        "FROM Notifications WHERE dismissed_at IS NULL AND group_id = ? ORDER BY ts;",
        -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    if (sqlite3_bind_int(stmt, 1, group_id) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    for (ret = sqlite3_step(stmt); ret == SQLITE_ROW; ret = sqlite3_step(stmt)) {
        int column = 0;
        int id = sqlite3_column_int(stmt, column++);
        const char *title = (const char *)sqlite3_column_text(stmt, column++);
        const char *created_at = (const char *)sqlite3_column_text(stmt, column++);
        const char *dismissed_at = (const char *)sqlite3_column_text(stmt, column++);
        int reminder_id = sqlite3_column_int(stmt, column++);
        int group_id = sqlite3_column_int(stmt, column++);
        da_append(ns, ((Notification) {
            .id           = id,
            .title        = title        ? temp_strdup(title)        : NULL,
            .created_at   = created_at   ? temp_strdup(created_at)   : NULL,
            .dismissed_at = dismissed_at ? temp_strdup(dismissed_at) : NULL,
            .reminder_id  = reminder_id,
            .group_id     = group_id,
        }));
    }

    if (ret != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

typedef struct {
    int notif_id;            // The id of a "Singleton" Notification in the Group. It does not make much sense if group_count > 0. In that case it's probably the id of the first one, but I wouldn't count on that
    const char *title;       // TODO: maybe in case of group_id > 0 the title should be the title of the corresponding reminder?
    const char *created_at;  // TODO: maybe in case of group_id > 0 the created_at should be the created_at of the latest notification?
    int reminder_id;
    int group_id;    // something that uniquely identifies a group of notifications and it is computed as ifnull(reminder_id, -id)
    int group_count; // the amount of notificatiosn in the group (must be always > 0)
} Grouped_Notification;

typedef struct {
    Grouped_Notification *items;
    size_t count;
    size_t capacity;
} Grouped_Notifications;

bool load_active_grouped_notifications(sqlite3 *db, Grouped_Notifications *notifs)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;

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

    int ret = sqlite3_prepare_v2(db,
        "SELECT id, title, datetime(created_at, 'localtime') as ts, reminder_id, ifnull(reminder_id, -id) as group_id, count(*) as group_count "
        "FROM Notifications WHERE dismissed_at IS NULL GROUP BY group_id ORDER BY ts;",
        -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    for (ret = sqlite3_step(stmt); ret == SQLITE_ROW; ret = sqlite3_step(stmt)) {
        int column = 0;
        int notif_id = sqlite3_column_int(stmt, column++);
        const char *title = temp_strdup((const char *)sqlite3_column_text(stmt, column++));
        const char *created_at = temp_strdup((const char *)sqlite3_column_text(stmt, column++));
        int reminder_id = sqlite3_column_int(stmt, column++);
        int group_id = sqlite3_column_int(stmt, column++);
        int group_count = sqlite3_column_int(stmt, column++);
        da_append(notifs, ((Grouped_Notification) {
            .notif_id = notif_id,
            .title = title,
            .created_at = created_at,
            .reminder_id = reminder_id,
            .group_id = group_id,
            .group_count = group_count,
        }));
    }

    if (ret != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    return result;
}

void display_grouped_notifications(Grouped_Notifications gns)
{
    for (size_t i = 0; i < gns.count; ++i) {
        Grouped_Notification *it = &gns.items[i];
        assert(it->group_count > 0);
        if (it->group_count == 1) {
            printf("%zu: %s (%s)\n", i, it->title, it->created_at);
        } else {
            printf("%zu: [%d] %s (%s)\n", i, it->group_count, it->title, it->created_at);
        }
    }
}

bool show_active_notifications(sqlite3 *db)
{
    bool result = true;
    Grouped_Notifications gns = {0};

    if (!load_active_grouped_notifications(db, &gns)) return_defer(false);
    display_grouped_notifications(gns);

defer:
    free(gns.items);
    return result;
}

bool show_expanded_notifications_by_index(sqlite3 *db, size_t index)
{
    bool result = true;

    Grouped_Notifications gns = {0};
    Notifications ns = {0};

    if (!load_active_grouped_notifications(db, &gns)) return_defer(false);
    if (index >= gns.count) {
        fprintf(stderr, "ERROR: invalid index\n");
        return false;
    }
    if (!load_active_notifications_of_group(db, gns.items[index].group_id, &ns)) return_defer(false);

    for (size_t i = 0; i < ns.count; ++i) {
        Notification *it = &ns.items[i];
        printf("%s (%s)\n", it->title, it->created_at);
    }

defer:
    free(gns.items);
    free(ns.items);
    return result;
}

bool dismiss_grouped_notification_by_group_id(sqlite3 *db, int group_id)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;

    int ret = sqlite3_prepare_v2(db,
            "UPDATE Notifications SET dismissed_at = CURRENT_TIMESTAMP "
            "WHERE dismissed_at is NULL AND ifnull(reminder_id, -id) = ?", -1,
            &stmt, NULL);
    if (ret != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    if (sqlite3_bind_int(stmt, 1, group_id) != SQLITE_OK) {
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

bool dismiss_grouped_notifications_by_indices_from_args(sqlite3 *db, int *how_many_dismissed, int argc, char **argv)
{
    bool result = true;

    Grouped_Notifications gns = {0};
    if (!load_active_grouped_notifications(db, &gns)) return_defer(false);
    while (argc > 0) {
        int index = atoi(shift(argv, argc));
        if (!(0 <= index && (size_t)index < gns.count)) {
            fprintf(stderr, "WARNING: %d is not a valid index of an active notification\n", index);
            continue;
        }
        if (!dismiss_grouped_notification_by_group_id(db, gns.items[index].group_id)) return_defer(false);
        if (how_many_dismissed) *how_many_dismissed += gns.items[index].group_count;
    }

defer:
    free(gns.items);
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

    for (ret = sqlite3_step(stmt); ret == SQLITE_ROW; ret = sqlite3_step(stmt)) {
        int id = sqlite3_column_int(stmt, 0);
        const char *title = temp_strdup((const char *)sqlite3_column_text(stmt, 1));
        const char *scheduled_at = temp_strdup((const char *)sqlite3_column_text(stmt, 2));
        const char *period = (const char *)sqlite3_column_text(stmt, 3);
        if (period != NULL) period = temp_strdup(period);
        da_append(reminders, ((Reminder) {
            .id = id,
            .title = title,
            .scheduled_at = scheduled_at,
            .period = period,
        }));
    }

    if (ret != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
defer:
    if (stmt) sqlite3_finalize(stmt);
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
    for (Period period = 0; period < COUNT_PERIODS; ++period) {
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

void render_index_page(String_Builder *sb, Grouped_Notifications notifs, Reminders reminders)
{
#define OUT(buf, size) sb_append_buf(sb, buf, size);
#define ESCAPED(cstr) sb_append_html_escaped_buf(sb, cstr, strlen(cstr));
#define INT(x) sb_append_cstr(sb, temp_sprintf("%d", (x)));
#define PAGE_BODY "index_page.h"
#define PAGE_TITLE
#include "root_page.h"
#undef PAGE_TITLE
#undef PAGE_BODY
#undef INT
#undef ESCAPED
#undef OUT
}

void render_error_page(String_Builder *sb, int error_code, const char *error_name)
{
#define OUT(buf, size) sb_append_buf(sb, buf, size);
#define ERROR_CODE sb_append_cstr(sb, temp_sprintf("%d", error_code));
#define ERROR_NAME sb_append_cstr(sb, error_name);
#define PAGE_BODY "error_page.h"
#define PAGE_TITLE sb_append_cstr(sb, temp_sprintf(" - %d - %s", error_code, error_name));
#include "root_page.h"
#undef PAGE_TITLE
#undef PAGE_BODY
#undef ERROR_CODE
#undef ERROR_NAME
#undef OUT
}

void render_notif_page(String_Builder *sb, Notification notif)
{
#define OUT(buf, size) sb_append_buf(sb, buf, size);
#define ESCAPED(cstr) sb_append_html_escaped_buf(sb, cstr, strlen(cstr));
#define INT(x) sb_append_cstr(sb, temp_sprintf("%d", (x)));
#define PAGE_BODY "notif_page.h"
#define PAGE_TITLE sb_append_cstr(sb, " - Notification - "); INT(notif.id);
#include "root_page.h"
#undef PAGE_TITLE
#undef PAGE_BODY
#undef INT
#undef OUT
#undef ESCAPED
}

void render_version_page(String_Builder *sb)
{
#define OUT(buf, size) sb_append_buf(sb, buf, size);
#define ESCAPED(cstr) sb_append_html_escaped_buf(sb, cstr, strlen(cstr));
#define PAGE_BODY "version_page.h"
#define PAGE_TITLE sb_append_cstr(sb, " - "); sb_append_cstr(sb, GIT_HASH);
#include "root_page.h"
#undef PAGE_TITLE
#undef PAGE_BODY
#undef ESCAPED
#undef OUT
}

sqlite3 *open_tore_db(void)
{
    sqlite3 *result = NULL;

    const char *home_path = getenv("HOME");
    if (home_path == NULL) {
        fprintf(stderr, "ERROR: No $HOME environment variable is setup. We need it to find the location of ~/"TORE_FILENAME" database.\n");
        return_defer(NULL);
    }

    const char *tore_path = temp_sprintf("%s/"TORE_FILENAME, home_path);

    int ret = sqlite3_open(tore_path, &result);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "ERROR: %s: %s\n", tore_path, sqlite3_errstr(ret));
        return_defer(NULL);
    }

    if (!create_schema(result, tore_path)) {
        sqlite3_close(result);
        return_defer(NULL);
    }

defer:
    return result;
}

typedef struct Command {
    const char *name;
    const char *description;
    const char *signature;
    bool (*run)(struct Command *self, const char *program_name, int argc, char **argv);
} Command;

typedef enum {
    DESCRIPTION_SHORT,
    DESCRIPTION_FULL,
} Description_Type;

void command_describe(Command command, const char *program_name, int pad, Description_Type description_type)
{
    printf("%*s%s %s", pad, "", program_name, command.name);
    if (command.signature) printf(" %s", command.signature);
    printf("\n");
    if (command.description) {
        switch (description_type) {
        case DESCRIPTION_SHORT: {
            String_View description = sv_from_cstr(command.description);
            String_View short_description = sv_chop_by_delim(&description, '\n');
            printf("%*s    "SV_Fmt"\n", pad, "", SV_Arg(short_description));
            if (sv_trim(description).count != 0)
            printf("%*s    ...\n", pad + 2, "");
        } break;
        case DESCRIPTION_FULL: {
            String_View description = sv_from_cstr(command.description);
            while (description.count > 0) {
                String_View line = sv_chop_by_delim(&description, '\n');
                printf("%*s    "SV_Fmt"\n", pad, "", SV_Arg(line));
            }
        } break;
        default: UNREACHABLE("description_type");
        }
    }
}

bool version_run(Command *self, const char *program_name, int argc, char **argv)
{
    UNUSED(self);
    UNUSED(program_name);
    UNUSED(argc);
    UNUSED(argv);
    fprintf(stderr, "TORE GIT HASH:     "GIT_HASH"\n");
    fprintf(stderr, "SQLITE3 VERSION:   "SQLITE_VERSION"\n");
    // TODO: bake build datetime into `tore version`
    return true;
}

bool checkout_run(Command *self, const char *program_name, int argc, char **argv)
{
    UNUSED(self);
    UNUSED(program_name);
    UNUSED(argc);
    UNUSED(argv);
    bool result = true;
    sqlite3 *db = open_tore_db();
    if (!db) return_defer(false);
    if (!txn_begin(db)) return_defer(false);
    if (!fire_off_reminders(db)) return_defer(false);
    if (!show_active_notifications(db)) return_defer(false);
    // TODO: show reminders that are about to fire off
    //   Maybe they should fire off a "warning" notification before doing the main one?
defer:
    if (db) {
        if (result) result = txn_commit(db);
        sqlite3_close(db);
    }
    return result;
}

bool noti_dismiss_run(Command *self, const char *program_name, int argc, char **argv)
{
    bool result = true;
    sqlite3 *db = NULL;
    if (argc <= 0) {
        fprintf(stderr, "Usage:\n");
        command_describe(*self, program_name, 2, DESCRIPTION_SHORT);
        fprintf(stderr, "ERROR: expected indices\n");
        return_defer(false);
    }

    db = open_tore_db();
    if (!db) return_defer(false);
    if (!txn_begin(db)) return_defer(false);

    int how_many_dismissed = 0;
    if (!dismiss_grouped_notifications_by_indices_from_args(db, &how_many_dismissed, argc, argv)) return_defer(false);
    if (!show_active_notifications(db)) return_defer(false);
    printf("Dismissed %d notifications\n", how_many_dismissed);
defer:
    if (db) {
        if (result) result = txn_commit(db);
        sqlite3_close(db);
    }
    return result;
}

typedef struct {
    int client_fd;
    Grouped_Notifications notifs;
    Reminders reminders;
    String_Builder request;
    String_Builder response;
    String_Builder body;
} Serve_Context;

void sc_reset(Serve_Context *sc)
{
    sc->notifs.count = 0;
    sc->reminders.count = 0;
    sc->body.count = 0;
    sc->response.count = 0;
    sc->request.count = 0;
}

Resource *find_resource(const char *file_path)
{
    for (size_t i = 0; i < resources_count; ++i) {
        if (strcmp(file_path, resources[i].file_path) == 0) {
            return &resources[i];
        }
    }
    return NULL;
}

bool write_entire_sv(int fd, String_View sv)
{
    String_View untransfered = sv;
    while (untransfered.count > 0) {
        ssize_t transfered = write(fd, untransfered.data, untransfered.count);
        if (transfered < 0) {
            fprintf(stderr, "ERROR: Could not write response: %s\n", strerror(errno));
            return false;
        }
        untransfered.data += transfered;
        untransfered.count -= transfered;
    }
    return true;
}

const char *http_reason_phrase_by_status_code(int status_code)
{
    // Taken from https://gist.github.com/josantonius/0a889ab6f18db2fcefda15a039613293
    static const char *reason_phrases[] = {
        [100] = "Continue",
        [101] = "Switching Protocols",
        [102] = "Processing",
        [103] = "Checkpoint",
        [200] = "OK",
        [201] = "Created",
        [202] = "Accepted",
        [203] = "Non-Authoritative Information",
        [204] = "No Content",
        [205] = "Reset Content",
        [206] = "Partial Content",
        [207] = "Multi-Status",
        [208] = "Already Reported",
        [300] = "Multiple Choices",
        [301] = "Moved Permanently",
        [302] = "Found",
        [303] = "See Other",
        [304] = "Not Modified",
        [305] = "Use Proxy",
        [306] = "Switch Proxy",
        [307] = "Temporary Redirect",
        [308] = "Permanent Redirect",
        [400] = "Bad Request",
        [401] = "Unauthorized",
        [402] = "Payment Required",
        [403] = "Forbidden",
        [404] = "Not Found",
        [405] = "Method Not Allowed",
        [406] = "Not Acceptable",
        [407] = "Proxy Authentication Required",
        [408] = "Request Time-out",
        [409] = "Conflict",
        [410] = "Gone",
        [411] = "Length Required",
        [412] = "Precondition Failed",
        [413] = "Request Entity Too Large",
        [414] = "Request-URI Too Long",
        [415] = "Unsupported Media Type",
        [416] = "Requested Range Not Satisfiable",
        [417] = "Expectation Failed",
        [418] = "I'm a teapot",
        [421] = "Unprocessable Entity",
        [422] = "Misdirected Request",
        [423] = "Locked",
        [424] = "Failed Dependency",
        [426] = "Upgrade Required",
        [428] = "Precondition Required",
        [429] = "Too Many Requests",
        [431] = "Request Header Fileds Too Large",
        [451] = "Unavailable For Legal Reasons",
        [500] = "Internal Server Error",
        [501] = "Not Implemented",
        [502] = "Bad Gateway",
        [503] = "Service Unavailable",
        [504] = "Gateway Timeout",
        [505] = "HTTP Version Not Supported",
        [506] = "Variant Also Negotiates",
        [507] = "Insufficient Storage",
        [508] = "Loop Detected",
        [509] = "Bandwidth Limit Exceeded",
        [510] = "Not Extended",
        [511] = "Network Authentication Required",
    };

    if (
        !((size_t)status_code < ARRAY_LEN(reason_phrases)) ||
        reason_phrases[status_code] == NULL
    ) return "Unknown";

    return reason_phrases[status_code];
}

void http_render_response(String_Builder *response, int status_code, const char *content_type, String_View body)
{
    sb_append_cstr(response, temp_sprintf("HTTP/1.0 %d %s\r\n", status_code, http_reason_phrase_by_status_code(status_code)));
    sb_append_cstr(response, temp_sprintf("Content-Type: %s\r\n", content_type));
    sb_append_cstr(response, temp_sprintf("Content-Length: %zu\r\n", body.count));
    sb_append_cstr(response, "Connection: close\r\n");
    sb_append_cstr(response, "\r\n");
    sb_append_buf(response, body.data, body.count);
}

void serve_error(Serve_Context *sc, int status_code)
{
    render_error_page(&sc->body, status_code, http_reason_phrase_by_status_code(status_code));
    http_render_response(&sc->response, status_code, "text/html", sb_to_sv(sc->body));
    UNUSED(write_entire_sv(sc->client_fd, sb_to_sv(sc->response)));
}

void serve_index(Serve_Context *sc)
{
    bool result = true;
    sqlite3 *db = open_tore_db();
    if (!db) return_defer(false);
    if (!txn_begin(db)) return_defer(false);

    if (!load_active_grouped_notifications(db, &sc->notifs)) {
        serve_error(sc, 500);
        return_defer(false);
    }

    if (!load_active_reminders(db, &sc->reminders)) {
        serve_error(sc, 500);
        return_defer(false);
    }

    render_index_page(&sc->body, sc->notifs, sc->reminders);
    http_render_response(&sc->response, 200, "text/html", sb_to_sv(sc->body));
    UNUSED(write_entire_sv(sc->client_fd, sb_to_sv(sc->response)));

defer:
    if (db) {
        if (result) result = txn_commit(db);
        sqlite3_close(db);
    }
}

bool serve_notif(Serve_Context *sc, int notif_id)
{
    bool result = true;
    sqlite3 *db = open_tore_db();
    if (!db) return_defer(false);
    if (!txn_begin(db)) return_defer(false);

    Notification notif = {0};
    int ret = load_notification_by_id(db, notif_id, &notif);
    if (ret < 0) {
        // something failed during request
        serve_error(sc, 500);
        return_defer(false);
    }
    if (ret == 0) {
        // notification was not found
        serve_error(sc, 404);
        return_defer(false);
    }

    render_notif_page(&sc->body, notif);
    http_render_response(&sc->response, 200, "text/html", sb_to_sv(sc->body));
    UNUSED(write_entire_sv(sc->client_fd, sb_to_sv(sc->response)));
defer:
    if (db) {
        if (result) result = txn_commit(db);
        sqlite3_close(db);
    }
    return result;
}

void serve_version(Serve_Context *sc)
{
    render_version_page(&sc->body);
    http_render_response(&sc->response, 200, "text/html", sb_to_sv(sc->body));
    UNUSED(write_entire_sv(sc->client_fd, sb_to_sv(sc->response)));
}

void serve_resource(Serve_Context *sc, const char *resource_path, const char *content_type)
{
    Resource *resource = find_resource(resource_path);
    if (!resource) {
        serve_error(sc, 404);
        return;
    }

    sb_append_buf(&sc->body, &bundle[resource->offset], resource->size);
    http_render_response(&sc->response, 200, content_type, sb_to_sv(sc->body));
    UNUSED(write_entire_sv(sc->client_fd, sb_to_sv(sc->response)));
}

void serve_request(Serve_Context *sc)
{
    // TODO: should `serve` fire off reminders?
    // TODO: log HTTP queries

    // <Status-Line>\r\n<Header>\r\n<Header>\r\n<Header>\r\n<Header>\r\n<Header>\r\n\r\n
    char buffer[1024];
    size_t cur = 0;
    String_View suffix = sv_from_parts("\r\n\r\n", 4);
    bool finish = false;
    ssize_t n = 0;
    do {
        n = read(sc->client_fd, buffer, sizeof(buffer));
        if (n <= 0) break;
        sb_append_buf(&sc->request, buffer, n);
        for (; cur < sc->request.count && !finish; cur += 1) {
            finish = nob_sv_starts_with(sv_from_parts(sc->request.items + cur, sc->request.count - cur), suffix);
        }
    } while (!finish);

    if (n < 0) {
        fprintf(stderr, "ERROR: could not read request: %s", strerror(errno));
        return;
    }

    // NULL terminating the request buffer, just in case we need to use some stupid libc functions
    // that only work with NULL-terminated strings.
    sb_append_null(&sc->request);

    String_View request = sb_to_sv(sc->request);
    String_View status_line = sv_trim(sv_chop_by_delim(&request, '\n'));
    String_View method = sv_trim(sv_chop_by_delim(&status_line, ' '));
    UNUSED(method);
    String_View uri =  sv_trim(sv_chop_by_delim(&status_line, ' '));

    if (sv_eq(uri, sv_from_cstr("/"))) {
        serve_index(sc);
        return;
    }
    if (sv_eq(uri, sv_from_cstr("/version"))) {
        serve_version(sc);
        return;
    }
    if (sv_eq(uri, sv_from_cstr("/favicon.ico"))) {
        serve_resource(sc, "./resources/images/tore.png", "image/png");
        return;
    }
    if (sv_eq(uri, sv_from_cstr("/css/reset.css"))) {
        serve_resource(sc, "./resources/css/reset.css", "text/css");
        return;
    }
    if (sv_eq(uri, sv_from_cstr("/css/main.css"))) {
        serve_resource(sc, "./resources/css/main.css", "text/css");
        return;
    }
    if (sv_eq(uri, sv_from_cstr("/urmom"))) {
        serve_error(sc, 413);
        return;
    }
    if (sv_starts_with(uri, sv_from_cstr("/notif/"))) {
        String_View notif_uri_prefix = sv_from_cstr("/notif/");
        uri.count -= notif_uri_prefix.count;
        uri.data += notif_uri_prefix.count;
        char *endptr = NULL;
        unsigned long notif_id = strtoul(uri.data, &endptr, 10);
        size_t id_len = endptr - uri.data;
        if (id_len == 0) {
            // id was not provided
            serve_error(sc, 404);
            return;
        }
        uri.count -= id_len;
        uri.data  += id_len;
        if (uri.count > 0) {
            // garbage after id
            serve_error(sc, 404);
            return;
        }
        UNUSED(serve_notif(sc, notif_id));
        return;
    }

    serve_error(sc, 404);
}

bool serve_run(Command *self, const char *program_name, int argc, char **argv)
{
    UNUSED(self);
    UNUSED(program_name);
    bool result = true;
    // NOTE: We are intentionally not listening to the external addresses, because we are using a
    // custom scuffed implementation of HTTP protocol, which is incomplete and possibly insecure.
    // The `serve` command is meant to be used only locally by a single person. At least for now.
    // We are doing it for the sake of simplicity, 'cause we don't have to ship an entire proper
    // HTTP server. Though, if you really want to, you can always slap some reverse proxy like nginx
    // on top of the `serve`.
    const char *addr = "127.0.0.1";
    uint16_t port = DEFAULT_SERVE_PORT;
    if (argc > 0) port = atoi(shift(argv, argc));

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "ERROR: Could not create socket epicly: %s\n", strerror(errno));
        return_defer(false);
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
        return_defer(false);
    }

    err = listen(server_fd, 69);
    if (err != 0) {
        fprintf(stderr, "ERROR: Could not listen to socket, it's too quiet: %s\n", strerror(errno));
        return_defer(false);
    }

    printf("Listening to http://%s:%d/\n", addr, port);

    Serve_Context sc = {0};
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_addrlen = 0;
        sc.client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addrlen);
        if (sc.client_fd < 0) {
            fprintf(stderr, "ERROR: Could not accept connection. This is unacceptable! %s\n", strerror(errno));
            continue;
        }

        UNUSED(serve_request(&sc));

        shutdown(sc.client_fd, SHUT_WR);
        char buffer[4096];
        while (read(sc.client_fd, buffer, sizeof(buffer)) > 0);
        close(sc.client_fd);
        sc_reset(&sc);
        temp_reset();
    }

    // TODO: The only way to stop the server is by SIGINT, but that probably doesn't close the db correctly.
    // So we probably should add a SIGINT handler specifically for this.

    UNREACHABLE("serve");

defer:
    // TODO: properly close the sockets on defer
    return result;
}

bool noti_run(Command *self, const char *program_name, int argc, char **argv)
{
    UNUSED(self);
    UNUSED(program_name);
    UNUSED(argc);
    UNUSED(argv);

    bool result = true;
    sqlite3 *db = NULL;

    db = open_tore_db();
    if (!db) return_defer(false);
    if (!txn_begin(db)) return_defer(false);
    if (!show_active_notifications(db)) return_defer(false);

defer:
    if (db) {
        if (result) result = txn_commit(db);
        sqlite3_close(db);
    }
    return result;
}

bool noti_new_run(Command *self, const char *program_name, int argc, char **argv)
{
    bool result = true;
    sqlite3 *db = NULL;
    String_Builder sb = {0};

    if (argc <= 0) {
        fprintf(stderr, "Usage:\n");
        command_describe(*self, program_name, 2, DESCRIPTION_SHORT);
        fprintf(stderr, "ERROR: expected title\n");
        return_defer(false);
    }

    db = open_tore_db();
    if (!db) return_defer(false);
    if (!txn_begin(db)) return_defer(false);

    for (bool pad = false; argc > 0; pad = true) {
        if (pad) sb_append_cstr(&sb, " ");
        sb_append_cstr(&sb, shift(argv, argc));
    }
    sb_append_null(&sb);
    const char *title = sb.items;

    if (!create_notification_with_title(db, title)) return_defer(false);
    if (!show_active_notifications(db)) return_defer(false);

defer:
    if (db) {
        if (result) result = txn_commit(db);
        sqlite3_close(db);
    }
    free(sb.items);
    return result;
}

bool remi_dismiss_run(Command *self, const char *program_name, int argc, char **argv)
{
    bool result = true;
    sqlite3 *db = NULL;
    if (argc <= 0) {
        fprintf(stderr, "Usage:\n");
        command_describe(*self, program_name, 2, DESCRIPTION_SHORT);
        fprintf(stderr, "ERROR: expected index\n");
        return_defer(false);
    }
    db = open_tore_db();
    if (!db) return_defer(false);
    if (!txn_begin(db)) return_defer(false);
    int number = atoi(shift(argv, argc));
    if (!remove_reminder_by_number(db, number)) return_defer(false);
    if (!show_active_reminders(db)) return_defer(false);
defer:
    if (db) {
        if (result) result = txn_commit(db);
        sqlite3_close(db);
    }
    return result;
}

bool remi_new_run(Command *self, const char *program_name, int argc, char **argv)
{
    bool result = true;
    sqlite3 *db = NULL;

    if (argc <= 0) {
        db = open_tore_db();
        if (!db) return_defer(false);
        if (!txn_begin(db)) return_defer(false);
        if (!show_active_reminders(db)) return_defer(false);
        return_defer(true);
    }

    const char *title = shift(argv, argc);
    if (argc <= 0) {
        fprintf(stderr, "Usage:\n");
        command_describe(*self, program_name, 2, DESCRIPTION_SHORT);
        fprintf(stderr, "ERROR: expected scheduled_at\n");
        return_defer(false);
    }

    // TODO: Allow the scheduled_at to be things like "today", "tomorrow", etc
    // TODO: research if it's possible to enforce the date format on the level of sqlite3 contraints
    const char *scheduled_at = shift(argv, argc);
    if (!verify_date_format(scheduled_at)) {
        fprintf(stderr, "ERROR: %s is not a valid date format\n", scheduled_at);
        return_defer(false);
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
            return_defer(false);
        }
        unparsed_period = endptr;
        period = period_by_tore_modifier(unparsed_period);
        if (period == PERIOD_NONE) {
            fprintf(stderr, "ERROR: Unknown period modifier `%s`. Expected modifiers are\n", unparsed_period);
            for (Period p = 0; p < COUNT_PERIODS; ++p) {
                Period_Modifier *pm = &tore_period_modifiers[p];
                fprintf(stderr, "    %lu%s  - means every %lu %s\n", period_length, pm->modifier, period_length, pm->name);
            }
            return_defer(false);
        }
    }

    db = open_tore_db();
    if (!db) return_defer(false);
    if (!txn_begin(db)) return_defer(false);
    if (!create_new_reminder(db, title, scheduled_at, period, period_length)) return_defer(false);
    if (!show_active_reminders(db)) return_defer(false);

defer:
    if (db) {
        if (result) result = txn_commit(db);
        sqlite3_close(db);
    }
    return result;
}

bool noti_expand_run(Command *self, const char *program_name, int argc, char **argv)
{
    bool result = true;
    sqlite3 *db = open_tore_db();
    if (!db) return_defer(false);
    if (!txn_begin(db)) return_defer(false);
    if (argc <= 0) {
        fprintf(stderr, "Usage:\n");
        command_describe(*self, program_name, 2, DESCRIPTION_SHORT);
        fprintf(stderr, "ERROR: no index is provided\n");
        return_defer(false);
    }
    int index = atoi(shift(argv, argc));
    if (!show_expanded_notifications_by_index(db, index)) return_defer(false);
defer:
    if (db) {
        if (result) result = txn_commit(db);
        sqlite3_close(db);
    }
    return result;
}

bool help_run(Command *self, const char *program_name, int argc, char **argv);

static Command commands[] = {
    {
        .name = "checkout",
        .signature = NULL,
        .description = "Fire off the Reminders if needed and show the current Notifications\n"
            "This is a default command that is executed when you just call Tore by itself.",
        .run = checkout_run,
    },
    {
        .name = "noti",
        .description = "Show the list of current Notifications, but unlike `checkout` do not fire them off.",
        .run = noti_run,
    },
    {
        .name = "noti:new",
        .signature = "<title...>",
        .description = "Add a new Notification manually.\n"
            "This Notification is not associated with any specific Reminder. You just create\n"
            "it in the moment to not forget something within the same day.",
        .run = noti_new_run,
    },
    {
        .name = "noti:dismiss",
        .signature = "<indices...>",
        .description = "Dismiss notifications by specified indices.",
        .run = noti_dismiss_run,
    },
    {
        .name = "noti:expand",
        .signature = "<index>",
        .description = "Expand a collapsed Group of Notifications by its index.\n"
            "When you have several undismissed Notifications generated by the same recurring\n"
            "Reminder they are usually collapsed into one in all the Notifications lists.\n"
            "To view the exact Notifications in the collapsed Group you can use this command.",
        .run = noti_expand_run,
    },
    // TODO: split remi:new and remi that just lists the reminders
    {
        .name = "remi:new",
        .signature = "[<title> <scheduled_at> [period]]",
        .description = "Schedule a reminder",
        .run = remi_new_run,
    },
    {
        .name = "remi:dismiss",
        .signature = "<index>",
        .description = "Remove a reminder by index",
        .run = remi_dismiss_run,
    },
    {
        .name = "serve",
        .signature = "[port]",
        .description = "Start up the Web Server. Default port is " STR(DEFAULT_SERVE_PORT) ".",
        .run = serve_run,
    },
    {
        .name = "help",
        .signature = "[command]",
        .description = "Show help messages for commands",
        .run = help_run,
    },
    {
        .name = "version",
        .signature = NULL,
        .description = "Show current version",
        .run = version_run,
    },
};

bool help_run(Command *self, const char *program_name, int argc, char **argv)
{
    UNUSED(self);
    const char *command_name = NULL;
    if (argc > 0) command_name = shift(argv, argc);

    if (command_name) {
        size_t match_count = 0;
        Command *last_match = NULL;
        for (size_t i = 0; i < ARRAY_LEN(commands); ++i) {
            if (sv_starts_with(sv_from_cstr(commands[i].name), sv_from_cstr(command_name))) {
                last_match = &commands[i];
                match_count += 1;
            }
        }
        switch (match_count) {
        case 0:
            fprintf(stderr, "ERROR: unknown command `%s`\n", command_name);
            return false;
        case 1:
            command_describe(*last_match, program_name, 0, DESCRIPTION_FULL);
            return true;
        default:
            printf("Commands matching prefix `%s`:\n", command_name);
            for (size_t i = 0; i < ARRAY_LEN(commands); ++i) {
                if (sv_starts_with(sv_from_cstr(commands[i].name), sv_from_cstr(command_name))) {
                    command_describe(commands[i], program_name, 2, DESCRIPTION_SHORT);
                    printf("\n");
                }
            }
            return true;
        }
    }

    printf("Usage:\n");
    printf("  %s [command] [command-arguments]\n", program_name);
    printf("\n");
    printf("Commands:\n");
    for (size_t i = 0; i < ARRAY_LEN(commands); ++i) {
        command_describe(commands[i], program_name, 2, DESCRIPTION_SHORT);
        printf("\n");
    }
    printf("The default command is `"DEFAULT_COMMAND"`.\n");
    return true;
}

int main(int argc, char **argv)
{
    int result = 0;

    srand(time(0));

    const char *program_name = shift(argv, argc);
    const char *command_name = DEFAULT_COMMAND;
    if (argc > 0) command_name = shift(argv, argc);

    for (size_t i = 0; i < ARRAY_LEN(commands); ++i) {
        if (strcmp(commands[i].name, command_name) == 0) {
            if (!commands[i].run(&commands[i], program_name, argc, argv)) return_defer(1);
            return_defer(0);
        }
    }

    fprintf(stderr, "ERROR: unknown command `%s`\n", command_name);
    return_defer(1);

defer:
    return result;
}

// TODO: `undo` command
// TODO: some way to turn Notification into a Reminder
// TODO: calendar output with the reminders

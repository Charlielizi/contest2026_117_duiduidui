/*
 * Report cards are a small, searchable index of operator-confirmed findings.
 * They complement (and never replace) the event/snapshot truth kept by
 * labtwin_service.c.
 */
#include "labtwin/labtwin_report.h"
#include "labtwin/labtwin.h"
#include "agent_compat.h"

#ifdef LABTWIN_REPORT_HOST_TEST
#ifndef LABTWIN_REPORT_ROOT
#define LABTWIN_REPORT_ROOT "/tmp/labtwin-report-test/reports"
#endif
#else
#define LABTWIN_REPORT_ROOT "/data/labtwin/reports"
#endif

#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <strings.h>
#include <sys/stat.h>
#include "cJSON.h"

#define REPORT_PATH_MAX 192
#define REPORT_MAX_COUNT 32
#define REPORT_MAX_BYTES (64 * 1024)
#define REPORT_CARD_MAX_BYTES 4096
#define REPORT_MAX_METRICS 12

static pthread_mutex_t g_report_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_report_counter;

static int ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? OK : ERROR;
    return (mkdir(path, 0700) == 0 || errno == EEXIST) ? OK : ERROR;
}

static int write_all(int fd, const char *text, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t n = write(fd, text + offset, length - offset);
        if (n <= 0) return ERROR;
        offset += (size_t)n;
    }
    return OK;
}

static int write_atomic(const char *path, cJSON *root)
{
    char temp[REPORT_PATH_MAX + 8];
    char *text = cJSON_PrintUnformatted(root);
    int fd;
    int result = ERROR;
    if (!text || strlen(text) > REPORT_CARD_MAX_BYTES) {
        free(text);
        return ERROR;
    }
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        int write_result = write_all(fd, text, strlen(text));
        int close_result = close(fd);
        if (write_result == OK && close_result == 0 &&
            rename(temp, path) == 0) {
            chmod(path, 0600);
            result = OK;
        }
    }
    if (result != OK) remove(temp);
    free(text);
    return result;
}

static cJSON *read_card(const char *path)
{
    struct stat st;
    FILE *file;
    char *text;
    cJSON *root;
    if (stat(path, &st) != 0 || st.st_size <= 0 ||
        (size_t)st.st_size > REPORT_CARD_MAX_BYTES) return NULL;
    file = fopen(path, "r");
    if (!file) return NULL;
    text = malloc((size_t)st.st_size + 1);
    if (!text) { fclose(file); return NULL; }
    text[fread(text, 1, (size_t)st.st_size, file)] = '\0';
    fclose(file);
    root = cJSON_Parse(text);
    free(text);
    return root;
}

static bool string_ok(cJSON *item, size_t maximum)
{
    const char *value = cJSON_GetStringValue(item);
    return value && value[0] && strlen(value) <= maximum &&
           !strchr(value, '\n') && !strchr(value, '\r');
}

static bool object_keys_allowed(cJSON *object, const char *const *allowed,
                                size_t allowed_count)
{
    if (!cJSON_IsObject(object)) return false;
    for (cJSON *child = object->child; child; child = child->next) {
        bool known = false;
        for (size_t i = 0; i < allowed_count; i++) {
            if (child->string && !strcmp(child->string, allowed[i])) {
                known = true;
                break;
            }
        }
        if (!known) return false;
    }
    return true;
}

static bool identifier_ok(const char *id)
{
    if (!id || !id[0] || strlen(id) > 31) return false;
    for (const char *p = id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    }
    return true;
}

static int response(char *output, size_t output_size, bool ok,
                    const char *code, const char *message, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    char *text;
    if (!root) { cJSON_Delete(data); return ERROR; }
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", message);
    if (data) cJSON_AddItemToObject(root, "data", data);
    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text || strlen(text) >= output_size) { free(text); return ERROR; }
    snprintf(output, output_size, "%s", text);
    free(text);
    return ok ? OK : ERROR;
}

static int storage_usage(size_t *bytes, int *count)
{
    DIR *dir = opendir(LABTWIN_REPORT_ROOT);
    struct dirent *entry;
    size_t total = 0;
    int cards = 0;
    if (!dir) { *bytes = 0; *count = 0; return OK; }
    while ((entry = readdir(dir)) != NULL) {
        char path[REPORT_PATH_MAX];
        struct stat st;
        size_t length = strlen(entry->d_name);
        if (entry->d_name[0] == '.' || length < 6 ||
            strcmp(entry->d_name + length - 5, ".json")) continue;
        /* d_name can be as long as NAME_MAX.  Build only paths that fit. */
        if (strlen(LABTWIN_REPORT_ROOT) + 1 + length + 1 > sizeof(path)) continue;
        memcpy(path, LABTWIN_REPORT_ROOT, strlen(LABTWIN_REPORT_ROOT));
        path[strlen(LABTWIN_REPORT_ROOT)] = '/';
        memcpy(path + strlen(LABTWIN_REPORT_ROOT) + 1, entry->d_name, length + 1);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            total += (size_t)st.st_size;
            cards++;
        }
    }
    closedir(dir);
    *bytes = total;
    *count = cards;
    return OK;
}

static bool validate_metrics(cJSON *metrics)
{
    static const char *const allowed[] = { "key", "value", "unit", "condition" };
    int count;
    if (!cJSON_IsArray(metrics) || (count = cJSON_GetArraySize(metrics)) < 1 ||
        count > REPORT_MAX_METRICS) return false;
    for (int i = 0; i < count; i++) {
        cJSON *metric = cJSON_GetArrayItem(metrics, i);
        cJSON *key = cJSON_GetObjectItemCaseSensitive(metric, "key");
        cJSON *value = cJSON_GetObjectItemCaseSensitive(metric, "value");
        cJSON *unit = cJSON_GetObjectItemCaseSensitive(metric, "unit");
        cJSON *condition = cJSON_GetObjectItemCaseSensitive(metric, "condition");
        if (!cJSON_IsObject(metric) || cJSON_GetArraySize(metric) != 4 ||
            !object_keys_allowed(metric, allowed, sizeof(allowed) / sizeof(allowed[0])) ||
            !string_ok(key, 48) ||
            !cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
            !string_ok(unit, 24) || !string_ok(condition, 128))
            return false;
    }
    return true;
}

static int verify_experiment(cJSON *experiment_id)
{
    char input[96];
    char result[256];
    if (!experiment_id) return OK;
    if (!string_ok(experiment_id, 31) || !identifier_ok(experiment_id->valuestring))
        return ERROR;
    snprintf(input, sizeof(input), "{\"experiment_id\":\"%s\"}",
             experiment_id->valuestring);
    return labtwin_experiment_get_json(input, result, sizeof(result));
}

int labtwin_report_init(void)
{
    return ensure_dir(LABTWIN_REPORT_ROOT);
}

int labtwin_report_create_json(const char *input, char *output,
                               size_t output_size, const char *source)
{
    static const char *const allowed[] = { "title", "summary", "tags", "metrics",
                                           "experiment_id", "supersedes_report_id" };
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *title = root ? cJSON_GetObjectItemCaseSensitive(root, "title") : NULL;
    cJSON *summary = root ? cJSON_GetObjectItemCaseSensitive(root, "summary") : NULL;
    cJSON *tags = root ? cJSON_GetObjectItemCaseSensitive(root, "tags") : NULL;
    cJSON *metrics = root ? cJSON_GetObjectItemCaseSensitive(root, "metrics") : NULL;
    cJSON *experiment_id = root ? cJSON_GetObjectItemCaseSensitive(root, "experiment_id") : NULL;
    cJSON *supersedes = root ? cJSON_GetObjectItemCaseSensitive(root, "supersedes_report_id") : NULL;
    cJSON *card = NULL;
    cJSON *data = NULL;
    char report_id[24];
    char path[REPORT_PATH_MAX];
    size_t bytes;
    int count;

    if (!root || !cJSON_IsObject(root) || cJSON_GetArraySize(root) < 4 ||
        cJSON_GetArraySize(root) > 6 || !string_ok(title, 96) ||
        !string_ok(summary, 512) || !cJSON_IsArray(tags) ||
        cJSON_GetArraySize(tags) > 8 || !validate_metrics(metrics) ||
        !object_keys_allowed(root, allowed, sizeof(allowed) / sizeof(allowed[0])) ||
        (experiment_id && verify_experiment(experiment_id) != OK) ||
        (supersedes && (!string_ok(supersedes, 31) ||
                        !identifier_ok(supersedes->valuestring)))) {
        cJSON_Delete(root);
        return response(output, output_size, false, "INVALID_REPORT", "invalid report card", NULL);
    }
    for (int i = 0; i < cJSON_GetArraySize(tags); i++) {
        if (!string_ok(cJSON_GetArrayItem(tags, i), 32)) {
            cJSON_Delete(root);
            return response(output, output_size, false, "INVALID_REPORT", "invalid report tag", NULL);
        }
    }
    pthread_mutex_lock(&g_report_lock);
    if (labtwin_report_init() != OK || storage_usage(&bytes, &count) != OK ||
        count >= REPORT_MAX_COUNT) {
        cJSON_Delete(root); pthread_mutex_unlock(&g_report_lock);
        return response(output, output_size, false, "REPORT_LIMIT", "report storage is full; export or clean up before adding another card", NULL);
    }
    if (supersedes) {
        cJSON *previous;
        snprintf(path, sizeof(path), LABTWIN_REPORT_ROOT "/%s.json",
                 supersedes->valuestring);
        previous = read_card(path);
        if (!previous) {
            cJSON_Delete(root); pthread_mutex_unlock(&g_report_lock);
            return response(output, output_size, false, "NOT_FOUND", "superseded report card was not found", NULL);
        }
        cJSON_Delete(previous);
    }
    do {
        snprintf(report_id, sizeof(report_id), "r%08" PRIx32 "%02" PRIx32,
                 (uint32_t)time(NULL), ++g_report_counter);
        snprintf(path, sizeof(path), LABTWIN_REPORT_ROOT "/%s.json", report_id);
    } while (access(path, F_OK) == 0);
    card = cJSON_CreateObject();
    cJSON_AddNumberToObject(card, "schema", 1);
    cJSON_AddStringToObject(card, "report_id", report_id);
    cJSON_AddStringToObject(card, "source", source ? source : "agent");
    cJSON_AddNumberToObject(card, "created_epoch", (double)time(NULL));
    cJSON_AddStringToObject(card, "title", title->valuestring);
    cJSON_AddStringToObject(card, "summary", summary->valuestring);
    cJSON_AddItemToObject(card, "tags", cJSON_Duplicate(tags, true));
    cJSON_AddItemToObject(card, "metrics", cJSON_Duplicate(metrics, true));
    if (experiment_id) cJSON_AddStringToObject(card, "experiment_id", experiment_id->valuestring);
    else cJSON_AddStringToObject(card, "experiment_id", "");
    if (supersedes) cJSON_AddStringToObject(card, "supersedes_report_id", supersedes->valuestring);
    else cJSON_AddStringToObject(card, "supersedes_report_id", "");
    {
        char *serialized = cJSON_PrintUnformatted(card);
        size_t needed = serialized ? strlen(serialized) : REPORT_CARD_MAX_BYTES + 1;
        free(serialized);
        if (bytes + needed > REPORT_MAX_BYTES || write_atomic(path, card) != OK) {
            cJSON_Delete(root); cJSON_Delete(card); pthread_mutex_unlock(&g_report_lock);
            return response(output, output_size, false, "REPORT_LIMIT", "report exceeds the local report budget", NULL);
        }
    }
    data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "report_id", report_id);
    cJSON_AddStringToObject(data, "experiment_id", experiment_id ? experiment_id->valuestring : "");
    cJSON_Delete(root); cJSON_Delete(card); pthread_mutex_unlock(&g_report_lock);
    return response(output, output_size, true, "OK", "report card saved", data);
}

static bool text_contains_ci(const char *text, const char *needle)
{
    size_t length;
    if (!needle || !needle[0]) return true;
    if (!text) return false;
    length = strlen(needle);
    for (; *text; text++) {
        size_t index = 0;
        while (index < length && text[index] &&
               tolower((unsigned char)text[index]) ==
               tolower((unsigned char)needle[index])) index++;
        if (index == length) return true;
    }
    return false;
}

static bool tags_match(cJSON *tags, const char *query)
{
    for (int i = 0; cJSON_IsArray(tags) && i < cJSON_GetArraySize(tags); i++) {
        const char *tag = cJSON_GetStringValue(cJSON_GetArrayItem(tags, i));
        if (text_contains_ci(tag, query)) return true;
    }
    return false;
}

static double matching_metric(cJSON *card, const char *metric_key,
                              bool *matched)
{
    cJSON *metrics = cJSON_GetObjectItemCaseSensitive(card, "metrics");
    *matched = metric_key == NULL || !metric_key[0];
    for (int i = 0; cJSON_IsArray(metrics) && i < cJSON_GetArraySize(metrics); i++) {
        cJSON *metric = cJSON_GetArrayItem(metrics, i);
        const char *key = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(metric, "key"));
        cJSON *value = cJSON_GetObjectItemCaseSensitive(metric, "value");
        if ((!metric_key || !metric_key[0] || (key && !strcasecmp(key, metric_key))) &&
            cJSON_IsNumber(value)) {
            *matched = true;
            return value->valuedouble;
        }
    }
    return 0;
}

static bool is_superseded(cJSON **cards, int count, const char *id)
{
    for (int i = 0; i < count; i++) {
        const char *old = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
            cards[i], "supersedes_report_id"));
        if (old && old[0] && !strcmp(old, id)) return true;
    }
    return false;
}

static cJSON *summary_card(cJSON *card, const char *metric_key)
{
    cJSON *out = cJSON_CreateObject();
    cJSON *metrics = cJSON_GetObjectItemCaseSensitive(card, "metrics");
    cJSON *selected = cJSON_AddArrayToObject(out, "metrics");
    const char *keys[] = { "report_id", "experiment_id", "title", "summary" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(card, keys[i]);
        if (cJSON_IsString(value)) cJSON_AddStringToObject(out, keys[i], value->valuestring);
    }
    cJSON *created = cJSON_GetObjectItemCaseSensitive(card, "created_epoch");
    if (cJSON_IsNumber(created)) cJSON_AddNumberToObject(out, "created_epoch", created->valuedouble);
    for (int i = 0; cJSON_IsArray(metrics) && i < cJSON_GetArraySize(metrics); i++) {
        cJSON *metric = cJSON_GetArrayItem(metrics, i);
        const char *key = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(metric, "key"));
        if (!metric_key || !metric_key[0] || (key && !strcasecmp(key, metric_key)))
            cJSON_AddItemToArray(selected, cJSON_Duplicate(metric, true));
    }
    return out;
}

int labtwin_report_search_json(const char *input, char *output,
                               size_t output_size)
{
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *query = root ? cJSON_GetObjectItemCaseSensitive(root, "query") : NULL;
    cJSON *experiment_id = root ? cJSON_GetObjectItemCaseSensitive(root, "experiment_id") : NULL;
    cJSON *metric_key = root ? cJSON_GetObjectItemCaseSensitive(root, "metric_key") : NULL;
    cJSON *minimum = root ? cJSON_GetObjectItemCaseSensitive(root, "min_value") : NULL;
    cJSON *maximum = root ? cJSON_GetObjectItemCaseSensitive(root, "max_value") : NULL;
    cJSON *from = root ? cJSON_GetObjectItemCaseSensitive(root, "from_epoch") : NULL;
    cJSON *to = root ? cJSON_GetObjectItemCaseSensitive(root, "to_epoch") : NULL;
    cJSON *sort = root ? cJSON_GetObjectItemCaseSensitive(root, "sort") : NULL;
    cJSON *limit_value = root ? cJSON_GetObjectItemCaseSensitive(root, "limit") : NULL;
    cJSON *cards[REPORT_MAX_COUNT] = { 0 };
    cJSON *matches[REPORT_MAX_COUNT] = { 0 };
    double scores[REPORT_MAX_COUNT] = { 0 };
    int count = 0, selected = 0, limit = 8;
    DIR *dir;
    struct dirent *entry;
    cJSON *data;

    if (!root || !cJSON_IsObject(root) ||
        (query && !string_ok(query, 96)) || (experiment_id && !string_ok(experiment_id, 31)) ||
        (metric_key && !string_ok(metric_key, 48)) ||
        (minimum && !cJSON_IsNumber(minimum)) || (maximum && !cJSON_IsNumber(maximum)) ||
        (from && !cJSON_IsNumber(from)) || (to && !cJSON_IsNumber(to)) ||
        (sort && (!string_ok(sort, 16) || (strcmp(sort->valuestring, "newest") &&
            strcmp(sort->valuestring, "metric_asc") && strcmp(sort->valuestring, "metric_desc")))) ||
        (limit_value && (!cJSON_IsNumber(limit_value) || limit_value->valueint < 1 || limit_value->valueint > 8)) ||
        (minimum && maximum && minimum->valuedouble > maximum->valuedouble) ||
        (from && to && from->valuedouble > to->valuedouble)) {
        cJSON_Delete(root);
        return response(output, output_size, false, "INVALID_SEARCH", "invalid report search", NULL);
    }
    if (limit_value) limit = limit_value->valueint;
    pthread_mutex_lock(&g_report_lock);
    dir = opendir(LABTWIN_REPORT_ROOT);
    while (dir && (entry = readdir(dir)) != NULL && count < REPORT_MAX_COUNT) {
        char path[REPORT_PATH_MAX];
        size_t n = strlen(entry->d_name);
        if (entry->d_name[0] == '.' || n < 6 || strcmp(entry->d_name + n - 5, ".json")) continue;
        if (strlen(LABTWIN_REPORT_ROOT) + 1 + n + 1 > sizeof(path)) continue;
        memcpy(path, LABTWIN_REPORT_ROOT, strlen(LABTWIN_REPORT_ROOT));
        path[strlen(LABTWIN_REPORT_ROOT)] = '/';
        memcpy(path + strlen(LABTWIN_REPORT_ROOT) + 1, entry->d_name, n + 1);
        cards[count] = read_card(path);
        if (cards[count]) count++;
    }
    if (dir) closedir(dir);
    data = cJSON_CreateObject();
    cJSON *results = cJSON_AddArrayToObject(data, "reports");
    for (int i = 0; i < count; i++) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cards[i], "report_id"));
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cards[i], "title"));
        const char *summary = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cards[i], "summary"));
        const char *experiment = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cards[i], "experiment_id"));
        cJSON *created = cJSON_GetObjectItemCaseSensitive(cards[i], "created_epoch");
        bool metric_found;
        double metric = matching_metric(cards[i], metric_key ? metric_key->valuestring : NULL, &metric_found);
        if (!id || is_superseded(cards, count, id) ||
            (query && !text_contains_ci(title, query->valuestring) &&
             !text_contains_ci(summary, query->valuestring) &&
             !tags_match(cJSON_GetObjectItemCaseSensitive(cards[i], "tags"), query->valuestring)) ||
            (experiment_id && (!experiment || strcmp(experiment, experiment_id->valuestring))) ||
            !metric_found || (minimum && metric < minimum->valuedouble) ||
            (maximum && metric > maximum->valuedouble) ||
            (from && (!cJSON_IsNumber(created) || created->valuedouble < from->valuedouble)) ||
            (to && (!cJSON_IsNumber(created) || created->valuedouble > to->valuedouble))) continue;
        scores[selected] = (sort && strcmp(sort->valuestring, "newest") != 0) ? metric :
            (cJSON_IsNumber(created) ? created->valuedouble : 0);
        matches[selected] = cards[i];
        selected++;
    }
    for (int i = 0; i < selected; i++) {
        for (int j = i + 1; j < selected; j++) {
            bool ascending = sort && !strcmp(sort->valuestring, "metric_asc");
            if ((ascending && scores[j] < scores[i]) || (!ascending && scores[j] > scores[i])) {
                cJSON *swap_card = matches[i]; double swap_score = scores[i];
                matches[i] = matches[j]; scores[i] = scores[j]; matches[j] = swap_card; scores[j] = swap_score;
            }
        }
    }
    for (int i = 0; i < selected && i < limit; i++)
        cJSON_AddItemToArray(results, summary_card(matches[i], metric_key ? metric_key->valuestring : NULL));
    cJSON_AddNumberToObject(data, "count", selected);
    for (int i = 0; i < count; i++) cJSON_Delete(cards[i]);
    pthread_mutex_unlock(&g_report_lock);
    cJSON_Delete(root);
    return response(output, output_size, true, "OK", "report search results", data);
}

int labtwin_report_get_json(const char *input, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *id = root ? cJSON_GetObjectItemCaseSensitive(root, "report_id") : NULL;
    char path[REPORT_PATH_MAX];
    cJSON *card;
    if (!root || !string_ok(id, 31) || !identifier_ok(id->valuestring)) {
        cJSON_Delete(root);
        return response(output, output_size, false, "INVALID_REPORT", "report_id is required", NULL);
    }
    snprintf(path, sizeof(path), LABTWIN_REPORT_ROOT "/%s.json", id->valuestring);
    pthread_mutex_lock(&g_report_lock);
    card = read_card(path);
    pthread_mutex_unlock(&g_report_lock);
    cJSON_Delete(root);
    if (!card) return response(output, output_size, false, "NOT_FOUND", "report card not found", NULL);
    return response(output, output_size, true, "OK", "report card", card);
}

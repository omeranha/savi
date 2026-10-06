#include "supabase_db.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include <string.h>

static const char *TAG = "supabase";

#define SUPABASE_RESPONSE_MAX 512
#define SUPABASE_URL_MAX      320
#define SUPABASE_PATH_MAX     192
#define SUPABASE_ALLOC_RETRIES 3

typedef struct {
    char body[SUPABASE_RESPONSE_MAX];
    size_t length;
    bool overflow;
} response_buffer_t;

static SemaphoreHandle_t s_lock;
static esp_http_client_handle_t s_client;
static response_buffer_t s_response;

static bool append_format(char *buffer, size_t capacity, size_t *length,
                          const char *format, ...)
{
    if (*length >= capacity) {
        return false;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *length, capacity - *length, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= capacity - *length) {
        return false;
    }
    *length += (size_t)written;
    return true;
}

static bool append_json_string(char *buffer, size_t capacity, size_t *length,
                               const char *value)
{
    if (!append_format(buffer, capacity, length, "\"")) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            if (!append_format(buffer, capacity, length, "\\%c", *p)) {
                return false;
            }
        } else if (*p < 0x20) {
            if (!append_format(buffer, capacity, length, "\\u%04x", *p)) {
                return false;
            }
        } else if (!append_format(buffer, capacity, length, "%c", *p)) {
            return false;
        }
    }
    return append_format(buffer, capacity, length, "\"");
}

static const char *json_skip_whitespace(const char *cursor)
{
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
        cursor++;
    }
    return cursor;
}

static bool json_parse_string(const char **cursor, const char *expected,
                              bool *matches)
{
    const char *p = *cursor;
    size_t expected_length = expected != NULL ? strlen(expected) : 0;
    size_t matched_length = 0;
    bool equal = expected != NULL;

    if (*p++ != '"') {
        return false;
    }
    while (*p != '"') {
        unsigned char decoded;
        if (*p == '\0' || (unsigned char)*p < 0x20) {
            return false;
        }
        if (*p == '\\') {
            p++;
            switch (*p++) {
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'u': {
                unsigned int codepoint = 0;
                for (int i = 0; i < 4; i++) {
                    char hex = *p++;
                    if (hex >= '0' && hex <= '9') {
                        codepoint = (codepoint << 4) | (unsigned int)(hex - '0');
                    } else if (hex >= 'a' && hex <= 'f') {
                        codepoint = (codepoint << 4) | (unsigned int)(hex - 'a' + 10);
                    } else if (hex >= 'A' && hex <= 'F') {
                        codepoint = (codepoint << 4) | (unsigned int)(hex - 'A' + 10);
                    } else {
                        return false;
                    }
                }
                decoded = codepoint <= 0x7f ? (unsigned char)codepoint : 0xff;
                break;
            }
            default:
                return false;
            }
        } else {
            decoded = (unsigned char)*p++;
        }

        if (!equal || matched_length >= expected_length ||
            decoded != (unsigned char)expected[matched_length]) {
            equal = false;
        } else {
            matched_length++;
        }
    }
    p++;

    *cursor = p;
    if (matches != NULL) {
        *matches = equal && matched_length == expected_length;
    }
    return true;
}

static bool json_skip_value(const char **cursor, unsigned int depth);

static bool json_skip_object(const char **cursor, unsigned int depth)
{
    const char *p = json_skip_whitespace(*cursor);
    if (*p++ != '{') {
        return false;
    }
    p = json_skip_whitespace(p);
    if (*p == '}') {
        *cursor = p + 1;
        return true;
    }

    for (;;) {
        if (!json_parse_string(&p, NULL, NULL)) {
            return false;
        }
        p = json_skip_whitespace(p);
        if (*p++ != ':') {
            return false;
        }
        p = json_skip_whitespace(p);
        if (!json_skip_value(&p, depth + 1)) {
            return false;
        }
        p = json_skip_whitespace(p);
        if (*p == '}') {
            *cursor = p + 1;
            return true;
        }
        if (*p++ != ',') {
            return false;
        }
        p = json_skip_whitespace(p);
    }
}

static bool json_skip_array(const char **cursor, unsigned int depth)
{
    const char *p = json_skip_whitespace(*cursor);
    if (*p++ != '[') {
        return false;
    }
    p = json_skip_whitespace(p);
    if (*p == ']') {
        *cursor = p + 1;
        return true;
    }

    for (;;) {
        if (!json_skip_value(&p, depth + 1)) {
            return false;
        }
        p = json_skip_whitespace(p);
        if (*p == ']') {
            *cursor = p + 1;
            return true;
        }
        if (*p++ != ',') {
            return false;
        }
        p = json_skip_whitespace(p);
    }
}

static bool json_skip_value(const char **cursor, unsigned int depth)
{
    if (depth > 16) {
        return false;
    }

    const char *p = json_skip_whitespace(*cursor);
    if (*p == '"') {
        if (!json_parse_string(&p, NULL, NULL)) {
            return false;
        }
    } else if (*p == '{') {
        if (!json_skip_object(&p, depth)) {
            return false;
        }
    } else if (*p == '[') {
        if (!json_skip_array(&p, depth)) {
            return false;
        }
    } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
        if (*p == '-') {
            p++;
        }
        if (*p == '0') {
            p++;
        } else {
            if (*p < '1' || *p > '9') {
                return false;
            }
            do {
                p++;
            } while (*p >= '0' && *p <= '9');
        }
        if (*p == '.') {
            p++;
            if (*p < '0' || *p > '9') {
                return false;
            }
            do {
                p++;
            } while (*p >= '0' && *p <= '9');
        }
        if (*p == 'e' || *p == 'E') {
            p++;
            if (*p == '+' || *p == '-') {
                p++;
            }
            if (*p < '0' || *p > '9') {
                return false;
            }
            do {
                p++;
            } while (*p >= '0' && *p <= '9');
        }
    } else if (strncmp(p, "true", 4) == 0) {
        p += 4;
    } else if (strncmp(p, "false", 5) == 0) {
        p += 5;
    } else if (strncmp(p, "null", 4) == 0) {
        p += 4;
    } else {
        return false;
    }

    *cursor = p;
    return true;
}

static bool json_parse_id_row(const char **cursor, uint32_t *id, bool *has_id)
{
    const char *p = json_skip_whitespace(*cursor);
    if (*p++ != '{') {
        return false;
    }
    *has_id = false;
    p = json_skip_whitespace(p);
    if (*p == '}') {
        *cursor = p + 1;
        return true;
    }

    for (;;) {
        bool is_id = false;
        if (!json_parse_string(&p, "esp_id", &is_id)) {
            return false;
        }
        p = json_skip_whitespace(p);
        if (*p++ != ':') {
            return false;
        }
        p = json_skip_whitespace(p);
        if (is_id) {
            if (*has_id || *p < '0' || *p > '9') {
                return false;
            }
            uint64_t parsed_id = 0;
            do {
                parsed_id = parsed_id * 10 + (unsigned int)(*p - '0');
                if (parsed_id > UINT32_MAX) {
                    return false;
                }
                p++;
            } while (*p >= '0' && *p <= '9');
            if (parsed_id == 0 || (*p != ',' && *p != '}' &&
                                   *p != ' ' && *p != '\t' &&
                                   *p != '\n' && *p != '\r')) {
                return false;
            }
            *id = (uint32_t)parsed_id;
            *has_id = true;
        } else if (!json_skip_value(&p, 1)) {
            return false;
        }

        p = json_skip_whitespace(p);
        if (*p == '}') {
            *cursor = p + 1;
            return true;
        }
        if (*p++ != ',') {
            return false;
        }
        p = json_skip_whitespace(p);
    }
}

static bool json_parse_id_array(const char *json, uint32_t *id,
                                bool *has_id, size_t *count)
{
    const char *p = json_skip_whitespace(json);
    if (*p++ != '[') {
        return false;
    }
    *count = 0;
    *has_id = false;
    p = json_skip_whitespace(p);
    if (*p != ']') {
        for (;;) {
            bool row_has_id = false;
            uint32_t row_id = 0;
            if (*p == '{') {
                if (!json_parse_id_row(&p, &row_id, &row_has_id)) {
                    return false;
                }
            } else if (!json_skip_value(&p, 1)) {
                return false;
            }
            if (*count == 0) {
                *has_id = row_has_id;
                *id = row_id;
            }
            (*count)++;
            p = json_skip_whitespace(p);
            if (*p == ']') {
                break;
            }
            if (*p++ != ',') {
                return false;
            }
            p = json_skip_whitespace(p);
        }
    }
    if (*p++ != ']') {
        return false;
    }
    p = json_skip_whitespace(p);
    return *p == '\0';
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    response_buffer_t *response = event->user_data;
    size_t data_len = (size_t)event->data_len;
    if (response->length + data_len >= sizeof(response->body)) {
        response->overflow = true;
        return ESP_ERR_NO_MEM;
    }

    memcpy(response->body + response->length, event->data, data_len);
    response->length += data_len;
    response->body[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t request_unlocked(esp_http_client_method_t method,
                                  const char *path,
                                  const char *json_body,
                                  const char *prefer,
                                  int *status)
{
    char url[SUPABASE_URL_MAX];
    const size_t base_len = strlen(CONFIG_SUPABASE_URL);
    const char *separator = (base_len > 0 && CONFIG_SUPABASE_URL[base_len - 1] == '/') ? "" : "/";
    int url_len = snprintf(url, sizeof(url), "%s%s%s", CONFIG_SUPABASE_URL, separator, path);
    if (url_len < 0 || (size_t)url_len >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(&s_response, 0, sizeof(s_response));
    esp_err_t err = esp_http_client_set_url(s_client, url);
    if (err != ESP_OK) {
        return err;
    }

    esp_http_client_set_method(s_client, method);
    err = esp_http_client_set_header(s_client, "apikey", CONFIG_SUPABASE_ANON_KEY);
    if (err == ESP_OK) {
        err = esp_http_client_set_header(s_client, "Authorization",
                                         "Bearer " CONFIG_SUPABASE_ANON_KEY);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(s_client, "Content-Type", "application/json");
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(s_client, "Prefer", prefer);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_post_field(s_client, json_body,
                                             json_body != NULL ? (int)strlen(json_body) : 0);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = esp_http_client_perform(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_response.overflow) {
        ESP_LOGE(TAG, "Supabase response exceeded %u bytes", (unsigned)sizeof(s_response.body) - 1);
        return ESP_ERR_INVALID_SIZE;
    }

    *status = esp_http_client_get_status_code(s_client);
    if (*status < 200 || *status >= 300) {
        ESP_LOGE(TAG, "Supabase returned HTTP %d: %s", *status,
                 s_response.length > 0 ? s_response.body : "(empty response)");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t supabase_db_init(void)
{
    if (CONFIG_SUPABASE_ANON_KEY[0] == '\0' ||
        strncmp(CONFIG_SUPABASE_URL, "https://", 8) != 0) {
        ESP_LOGE(TAG, "Configure an HTTPS Supabase URL and anon key in menuconfig");
        return ESP_ERR_INVALID_ARG;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    response_buffer_t *response = &s_response;
    esp_http_client_config_t config = {
        .url = CONFIG_SUPABASE_URL,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = response,
    };
    s_client = esp_http_client_init(&config);
    if (s_client == NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Supabase HTTPS client ready for %s", CONFIG_SUPABASE_URL);
    return ESP_OK;
}

esp_err_t supabase_db_update_bracelete(const sensor_data_msg_t *msg)
{
    if (msg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!isfinite(msg->temperature_c) || !isfinite(msg->ax) ||
        !isfinite(msg->ay) || !isfinite(msg->az) || !isfinite(msg->gx) ||
        !isfinite(msg->gy) || !isfinite(msg->gz)) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now = time(NULL);
    struct tm local_time;
    char timestamp[32];
    size_t timestamp_length;
    if (now == (time_t)-1 || localtime_r(&now, &local_time) == NULL ||
        (timestamp_length = strftime(timestamp, sizeof(timestamp),
                                     "%Y-%m-%dT%H:%M:%S", &local_time)) != 19 ||
        snprintf(timestamp + timestamp_length, sizeof(timestamp) - timestamp_length,
                 "-03:00") != 6) {
        ESP_LOGE(TAG, "Failed to get current synchronized timestamp");
        return ESP_FAIL;
    }

    char json[SUPABASE_RESPONSE_MAX];
    int json_len = snprintf(json, sizeof(json),
                            "{\"atualizado_em\":\"%s\",\"temperatura\":%.9g,"
                            "\"bpm\":%u,\"spo2\":%u,"
                            "\"acc_x\":%.9g,\"acc_y\":%.9g,\"acc_z\":%.9g,"
                            "\"gyro_x\":%.9g,\"gyro_y\":%.9g,\"gyro_z\":%.9g}",
                            timestamp, (double)msg->temperature_c, (unsigned)msg->hr_bpm,
                            (unsigned)msg->spo2_pct, (double)msg->ax, (double)msg->ay,
                            (double)msg->az, (double)msg->gx, (double)msg->gy,
                            (double)msg->gz);
    if (json_len < 0 || (size_t)json_len >= sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char path[SUPABASE_PATH_MAX];
    int path_len = snprintf(path, sizeof(path),
                            "rest/v1/braceletes?esp_id=eq.%" PRIu32, msg->esp_id);
    if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int status = 0;
    esp_err_t err = request_unlocked(HTTP_METHOD_PATCH, path, json, "return=minimal", &status);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t supabase_db_insert_alerta(uint32_t esp_id, const char *tipo, float valor)
{
    if (tipo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!isfinite(valor)) {
        return ESP_ERR_INVALID_ARG;
    }
    char json[SUPABASE_RESPONSE_MAX];
    size_t json_len = 0;
    if (!append_format(json, sizeof(json), &json_len, "{\"esp_id\":%" PRIu32 ",\"tipo\":",
                       esp_id) ||
        !append_json_string(json, sizeof(json), &json_len, tipo) ||
        !append_format(json, sizeof(json), &json_len, ",\"valor\":%.9g}",
                       (double)valor)) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int status = 0;
    esp_err_t err = request_unlocked(HTTP_METHOD_POST, "rest/v1/alertas", json,
                                     "return=minimal", &status);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t supabase_db_alloc_esp_id(uint32_t *esp_id)
{
    if (esp_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int attempt = 0; attempt < SUPABASE_ALLOC_RETRIES; attempt++) {
        int status = 0;
        err = request_unlocked(HTTP_METHOD_GET,
                               "rest/v1/braceletes?select=esp_id&em_uso=eq.false&order=esp_id.asc&limit=1",
                               NULL, "return=minimal", &status);
        if (err != ESP_OK) {
            break;
        }

        uint32_t id = 0;
        bool has_id = false;
        size_t row_count = 0;
        if (!json_parse_id_array(s_response.body, &id, &has_id, &row_count)) {
            ESP_LOGE(TAG, "Invalid ID allocation response");
            err = ESP_FAIL;
            break;
        }
        if (row_count == 0 || !has_id) {
            err = row_count == 0 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
            break;
        }

        char path[SUPABASE_PATH_MAX];
        int path_len = snprintf(path, sizeof(path),
                                "rest/v1/braceletes?esp_id=eq.%" PRIu32 "&em_uso=eq.false&select=esp_id",
                                id);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        err = request_unlocked(HTTP_METHOD_PATCH, path, "{\"em_uso\":true}",
                               "return=representation", &status);
        if (err != ESP_OK) {
            break;
        }

        uint32_t claimed_id = 0;
        bool claimed_has_id = false;
        size_t claimed_count = 0;
        bool valid_response = json_parse_id_array(s_response.body, &claimed_id,
                                                   &claimed_has_id, &claimed_count);
        if (valid_response && claimed_count == 1) {
            *esp_id = id;
            err = ESP_OK;
            break;
        }
        err = ESP_ERR_NOT_FOUND;
    }
    xSemaphoreGive(s_lock);

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "No unused esp_id could be allocated");
    }
    return err;
}

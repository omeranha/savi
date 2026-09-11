#include "mysql_db.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "mysql";

#define MYSQL_RECV_TIMEOUT_MS 5000
#define MYSQL_PKT_MAX         2048

/* Edit these to match your schema. Rows are selected by the bracelet ESP ID from the packet. */
#define SQL_UPDATE_BRACELETE \
    "UPDATE braceletes SET temperatura=%.2f, bpm=%u, spo2=%u, " \
    "acc_x=%.3f, acc_y=%.3f, acc_z=%.3f, gyro_x=%.3f, gyro_y=%.3f, gyro_z=%.3f " \
    "WHERE esp_id=%u"

#define SQL_INSERT_ALERTA \
    "INSERT INTO alertas (esp_id, tipo, valor) VALUES (%u, '%s', %.3f)"

#define SQL_ALLOC_ID \
    "SELECT esp_id FROM braceletes WHERE em_uso=0 ORDER BY esp_id ASC LIMIT 1"

#define SQL_MARK_ID_USED \
    "UPDATE braceletes SET em_uso=1 WHERE esp_id=%u AND em_uso=0"

#define CLIENT_LONG_PASSWORD                  0x00000001
#define CLIENT_LONG_FLAG                      0x00000004
#define CLIENT_CONNECT_WITH_DB                0x00000008
#define CLIENT_PROTOCOL_41                    0x00000200
#define CLIENT_TRANSACTIONS                   0x00002000
#define CLIENT_SECURE_CONNECTION              0x00008000
#define CLIENT_PLUGIN_AUTH                    0x00080000

static SemaphoreHandle_t s_lock;
static int s_sock = -1;
static uint8_t s_seq;

static int sha1(const uint8_t *in, size_t len, uint8_t out[20])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (info == NULL) {
        return -1;
    }
    return mbedtls_md(info, in, len, out);
}

static void mysql_scramble(const char *password, const uint8_t *salt, uint8_t out[20])
{
    uint8_t hash1[20];
    uint8_t hash2[20];
    uint8_t hash3[20];
    uint8_t stage[40];

    sha1((const uint8_t *)password, strlen(password), hash1);
    sha1(hash1, 20, hash2);
    memcpy(stage, salt, 20);
    memcpy(stage + 20, hash2, 20);
    sha1(stage, 40, hash3);
    for (int i = 0; i < 20; i++) {
        out[i] = hash1[i] ^ hash3[i];
    }
}

static int sock_send_all(int sock, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int sock_recv_all(int sock, uint8_t *data, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = recv(sock, data + got, len - got, 0);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

static int mysql_send_packet(int sock, const uint8_t *payload, uint32_t len)
{
    uint8_t hdr[4] = {
        (uint8_t)(len & 0xFF),
        (uint8_t)((len >> 8) & 0xFF),
        (uint8_t)((len >> 16) & 0xFF),
        s_seq++,
    };
    if (sock_send_all(sock, hdr, 4) != 0) {
        return -1;
    }
    if (len > 0 && sock_send_all(sock, payload, len) != 0) {
        return -1;
    }
    return 0;
}

static int mysql_recv_packet(int sock, uint8_t *buf, size_t buf_len, uint32_t *out_len)
{
    uint8_t hdr[4];
    if (sock_recv_all(sock, hdr, 4) != 0) {
        return -1;
    }
    uint32_t len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16);
    s_seq = (uint8_t)(hdr[3] + 1);
    if (len >= buf_len) {
        return -1;
    }
    if (len > 0 && sock_recv_all(sock, buf, len) != 0) {
        return -1;
    }
    buf[len] = 0;
    *out_len = len;
    return 0;
}

static void mysql_close_unlocked(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

static esp_err_t mysql_connect_unlocked(void)
{
    mysql_close_unlocked();
    s_seq = 0;

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%d", CONFIG_MYSQL_PORT);

    int err = getaddrinfo(CONFIG_MYSQL_HOST, port, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS failed for %s", CONFIG_MYSQL_HOST);
        return ESP_FAIL;
    }

    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        freeaddrinfo(res);
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = MYSQL_RECV_TIMEOUT_MS / 1000,
        .tv_usec = (MYSQL_RECV_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "Connect to %s:%d failed", CONFIG_MYSQL_HOST, CONFIG_MYSQL_PORT);
        close(sock);
        freeaddrinfo(res);
        return ESP_FAIL;
    }
    freeaddrinfo(res);

    uint8_t pkt[MYSQL_PKT_MAX];
    uint32_t pkt_len = 0;
    if (mysql_recv_packet(sock, pkt, sizeof(pkt) - 1, &pkt_len) != 0 || pkt_len < 20) {
        close(sock);
        return ESP_FAIL;
    }
    if (pkt[0] != 0x0A) {
        ESP_LOGE(TAG, "Unsupported MySQL protocol");
        close(sock);
        return ESP_FAIL;
    }

    const uint8_t *p = pkt + 1;
    while (p < pkt + pkt_len && *p != 0) {
        p++;
    }
    p++; /* skip server version */
    if (p + 13 > pkt + pkt_len) {
        close(sock);
        return ESP_FAIL;
    }
    p += 4; /* thread id */

    uint8_t salt[20] = {0};
    memcpy(salt, p, 8);
    p += 8;
    p += 1; /* filler */
    p += 2; /* cap lower */
    if (p + 13 > pkt + pkt_len) {
        close(sock);
        return ESP_FAIL;
    }
    p += 1; /* charset */
    p += 2; /* status */
    p += 2; /* cap upper */
    p += 1; /* auth data len */
    p += 10; /* reserved */
    size_t rest = (size_t)((pkt + pkt_len) - p);
    if (rest >= 12) {
        memcpy(salt + 8, p, 12);
    }

    uint8_t token[20];
    mysql_scramble(CONFIG_MYSQL_PASSWORD, salt, token);

    uint8_t hs[512];
    uint32_t n = 0;
    uint32_t caps = CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_CONNECT_WITH_DB |
                    CLIENT_PROTOCOL_41 | CLIENT_TRANSACTIONS | CLIENT_SECURE_CONNECTION |
                    CLIENT_PLUGIN_AUTH;
    hs[n++] = (uint8_t)(caps);
    hs[n++] = (uint8_t)(caps >> 8);
    hs[n++] = (uint8_t)(caps >> 16);
    hs[n++] = (uint8_t)(caps >> 24);
    hs[n++] = 0xFF;
    hs[n++] = 0xFF;
    hs[n++] = 0x00;
    hs[n++] = 0x00; /* max packet */
    hs[n++] = 0x21; /* utf8 */
    memset(hs + n, 0, 23);
    n += 23;

    size_t user_len = strlen(CONFIG_MYSQL_USER);
    memcpy(hs + n, CONFIG_MYSQL_USER, user_len);
    n += user_len;
    hs[n++] = 0;

    hs[n++] = 20;
    memcpy(hs + n, token, 20);
    n += 20;

    size_t db_len = strlen(CONFIG_MYSQL_DATABASE);
    memcpy(hs + n, CONFIG_MYSQL_DATABASE, db_len);
    n += db_len;
    hs[n++] = 0;

    const char *plugin = "mysql_native_password";
    size_t plugin_len = strlen(plugin);
    memcpy(hs + n, plugin, plugin_len);
    n += plugin_len;
    hs[n++] = 0;

    if (mysql_send_packet(sock, hs, n) != 0) {
        close(sock);
        return ESP_FAIL;
    }

    if (mysql_recv_packet(sock, pkt, sizeof(pkt) - 1, &pkt_len) != 0 || pkt_len < 1) {
        close(sock);
        return ESP_FAIL;
    }
    if (pkt[0] == 0xFF) {
        ESP_LOGE(TAG, "MySQL auth error: %s", pkt_len > 9 ? (char *)(pkt + 9) : "");
        close(sock);
        return ESP_FAIL;
    }
    if (pkt[0] != 0x00 && pkt[0] != 0xFE) {
        ESP_LOGE(TAG, "Unexpected auth response 0x%02X", pkt[0]);
        close(sock);
        return ESP_FAIL;
    }

    s_sock = sock;
    ESP_LOGI(TAG, "Connected to MySQL %s/%s", CONFIG_MYSQL_HOST, CONFIG_MYSQL_DATABASE);
    return ESP_OK;
}

static bool mysql_is_eof(const uint8_t *buf, uint32_t len)
{
    return (len > 0 && buf[0] == 0xFE && len < 9);
}

static int mysql_read_lenenc(const uint8_t *p, uint32_t remain, uint64_t *val, uint32_t *used)
{
    if (remain < 1) {
        return -1;
    }
    uint8_t fb = p[0];
    if (fb < 0xFB) {
        *val = fb;
        *used = 1;
        return 0;
    }
    if (fb == 0xFC && remain >= 3) {
        *val = (uint64_t)p[1] | ((uint64_t)p[2] << 8);
        *used = 3;
        return 0;
    }
    if (fb == 0xFD && remain >= 4) {
        *val = (uint64_t)p[1] | ((uint64_t)p[2] << 8) | ((uint64_t)p[3] << 16);
        *used = 4;
        return 0;
    }
    return -1;
}

static esp_err_t mysql_send_query(const char *sql)
{
    if (s_sock < 0 && mysql_connect_unlocked() != ESP_OK) {
        return ESP_FAIL;
    }

    size_t sql_len = strlen(sql);
    if (sql_len + 1 >= MYSQL_PKT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t buf[MYSQL_PKT_MAX];
    buf[0] = 0x03;
    memcpy(buf + 1, sql, sql_len);
    s_seq = 0;
    if (mysql_send_packet(s_sock, buf, (uint32_t)(sql_len + 1)) != 0) {
        mysql_close_unlocked();
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t mysql_drain_resultset(uint8_t *buf, uint32_t first_len)
{
    uint64_t cols = 0;
    uint32_t used = 0;
    if (mysql_read_lenenc(buf, first_len, &cols, &used) != 0 || cols == 0) {
        return ESP_FAIL;
    }
    for (uint64_t i = 0; i < cols; i++) {
        uint32_t pkt_len = 0;
        if (mysql_recv_packet(s_sock, buf, MYSQL_PKT_MAX - 1, &pkt_len) != 0) {
            return ESP_FAIL;
        }
    }
    /* Protocol 41: EOF, then rows, then EOF (or OK if empty after first EOF). */
    uint32_t pkt_len = 0;
    if (mysql_recv_packet(s_sock, buf, MYSQL_PKT_MAX - 1, &pkt_len) != 0) {
        return ESP_FAIL;
    }
    while (!mysql_is_eof(buf, pkt_len) && buf[0] != 0x00) {
        if (buf[0] == 0xFF) {
            return ESP_FAIL;
        }
        if (mysql_recv_packet(s_sock, buf, MYSQL_PKT_MAX - 1, &pkt_len) != 0 || pkt_len < 1) {
            return ESP_FAIL;
        }
    }
    if (mysql_is_eof(buf, pkt_len)) {
        while (1) {
            if (mysql_recv_packet(s_sock, buf, MYSQL_PKT_MAX - 1, &pkt_len) != 0 || pkt_len < 1) {
                return ESP_FAIL;
            }
            if (buf[0] == 0xFF) {
                return ESP_FAIL;
            }
            if (mysql_is_eof(buf, pkt_len) || buf[0] == 0x00) {
                break;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t mysql_query_unlocked(const char *sql)
{
    uint8_t buf[MYSQL_PKT_MAX];
    uint32_t pkt_len = 0;

    if (mysql_send_query(sql) != ESP_OK) {
        return ESP_FAIL;
    }
    if (mysql_recv_packet(s_sock, buf, sizeof(buf) - 1, &pkt_len) != 0 || pkt_len < 1) {
        mysql_close_unlocked();
        return ESP_FAIL;
    }
    if (buf[0] == 0xFF) {
        ESP_LOGE(TAG, "Query error: %s", pkt_len > 9 ? (char *)(buf + 9) : sql);
        return ESP_FAIL;
    }
    if (buf[0] != 0x00 && buf[0] != 0xFE) {
        if (mysql_drain_resultset(buf, pkt_len) != ESP_OK) {
            mysql_close_unlocked();
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "SQL ok: %s", sql);
    return ESP_OK;
}

static esp_err_t mysql_select_u32_unlocked(const char *sql, uint32_t *out)
{
    uint8_t buf[MYSQL_PKT_MAX];
    uint32_t pkt_len = 0;

    if (mysql_send_query(sql) != ESP_OK) {
        return ESP_FAIL;
    }
    if (mysql_recv_packet(s_sock, buf, sizeof(buf) - 1, &pkt_len) != 0 || pkt_len < 1) {
        mysql_close_unlocked();
        return ESP_FAIL;
    }
    if (buf[0] == 0xFF) {
        ESP_LOGE(TAG, "Query error: %s", pkt_len > 9 ? (char *)(buf + 9) : sql);
        return ESP_FAIL;
    }
    if (buf[0] == 0x00) {
        return ESP_ERR_NOT_FOUND;
    }

    uint64_t cols = 0;
    uint32_t used = 0;
    if (mysql_read_lenenc(buf, pkt_len, &cols, &used) != 0) {
        return ESP_FAIL;
    }
    for (uint64_t i = 0; i < cols; i++) {
        if (mysql_recv_packet(s_sock, buf, sizeof(buf) - 1, &pkt_len) != 0) {
            mysql_close_unlocked();
            return ESP_FAIL;
        }
    }

    /* Field EOF, then row data, then final EOF. */
    if (mysql_recv_packet(s_sock, buf, sizeof(buf) - 1, &pkt_len) != 0 || pkt_len < 1) {
        mysql_close_unlocked();
        return ESP_FAIL;
    }
    if (mysql_is_eof(buf, pkt_len) || buf[0] == 0x00) {
        if (mysql_recv_packet(s_sock, buf, sizeof(buf) - 1, &pkt_len) != 0 || pkt_len < 1) {
            mysql_close_unlocked();
            return ESP_FAIL;
        }
    }

    bool have_row = false;
    while (!mysql_is_eof(buf, pkt_len) && buf[0] != 0x00) {
        if (buf[0] == 0xFF) {
            return ESP_FAIL;
        }
        if (!have_row) {
            uint32_t used_row = 0;
            uint64_t flen = 0;
            if (mysql_read_lenenc(buf, pkt_len, &flen, &used_row) == 0 && used_row + flen <= pkt_len) {
                char num[16] = {0};
                size_t n = (flen < sizeof(num) - 1) ? (size_t)flen : sizeof(num) - 1;
                memcpy(num, buf + used_row, n);
                *out = (uint32_t)strtoul(num, NULL, 10);
                have_row = true;
            }
        }
        if (mysql_recv_packet(s_sock, buf, sizeof(buf) - 1, &pkt_len) != 0 || pkt_len < 1) {
            mysql_close_unlocked();
            return ESP_FAIL;
        }
    }

    return have_row ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t mysql_db_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return (s_lock != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mysql_db_update_bracelete(const sensor_data_msg_t *msg)
{
    char sql[512];
    snprintf(sql, sizeof(sql), SQL_UPDATE_BRACELETE,
             msg->temperature_c, msg->hr_bpm, msg->spo2_pct,
             msg->ax, msg->ay, msg->az, msg->gx, msg->gy, msg->gz, msg->esp_id);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = mysql_query_unlocked(sql);
    if (err != ESP_OK) {
        mysql_close_unlocked();
        err = mysql_query_unlocked(sql);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t mysql_db_insert_alerta(uint32_t esp_id, const char *tipo, float valor)
{
    char sql[256];
    snprintf(sql, sizeof(sql), SQL_INSERT_ALERTA, esp_id, tipo, valor);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = mysql_query_unlocked(sql);
    if (err != ESP_OK) {
        mysql_close_unlocked();
        err = mysql_query_unlocked(sql);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t mysql_db_alloc_esp_id(uint32_t *esp_id)
{
    uint32_t id = 0;
    char sql[96];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = mysql_select_u32_unlocked(SQL_ALLOC_ID, &id);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        mysql_close_unlocked();
        err = mysql_select_u32_unlocked(SQL_ALLOC_ID, &id);
    }
    if (err == ESP_OK) {
        snprintf(sql, sizeof(sql), SQL_MARK_ID_USED, id);
        err = mysql_query_unlocked(sql);
        if (err == ESP_OK) {
            *esp_id = id;
        }
    }
    xSemaphoreGive(s_lock);

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "No unused esp_id in braceletes (em_uso=0)");
    }
    return err;
}

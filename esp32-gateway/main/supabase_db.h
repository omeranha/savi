#ifndef SUPABASE_DB_H
#define SUPABASE_DB_H

#include "esp_err.h"
#include "main.h"

esp_err_t supabase_db_init(void);
esp_err_t supabase_db_update_bracelete(const sensor_data_msg_t *msg);
esp_err_t supabase_db_insert_alerta(uint32_t esp_id, const char *tipo, float valor);
esp_err_t supabase_db_alloc_esp_id(uint32_t *esp_id);

#endif

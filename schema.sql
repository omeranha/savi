-- SAVI MySQL schema
-- Matches the gateway queries in esp32-gateway/main/mysql_db.c
--
--   SELECT esp_id FROM braceletes WHERE em_uso=0 ...
--   UPDATE braceletes SET em_uso=1 WHERE esp_id=?
--   UPDATE braceletes SET temperatura, bpm, spo2, acc_*, gyro_* WHERE esp_id=?
--   INSERT INTO alertas (esp_id, tipo, valor) ...
--
-- Import:
--   mysql -u root -p < schema.sql

CREATE DATABASE IF NOT EXISTS savi
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE savi;

CREATE TABLE IF NOT EXISTS braceletes (
    esp_id          INT UNSIGNED    NOT NULL,
    em_uso          TINYINT(1)      NOT NULL DEFAULT 0,
    temperatura     FLOAT           NULL,
    bpm             TINYINT UNSIGNED NULL,
    spo2            TINYINT UNSIGNED NULL,
    acc_x           FLOAT           NULL,
    acc_y           FLOAT           NULL,
    acc_z           FLOAT           NULL,
    gyro_x          FLOAT           NULL,
    gyro_y          FLOAT           NULL,
    gyro_z          FLOAT           NULL,
    atualizado_em   DATETIME        NULL DEFAULT NULL,
    PRIMARY KEY (esp_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS alertas (
    id          INT UNSIGNED    NOT NULL AUTO_INCREMENT,
    esp_id      INT UNSIGNED    NOT NULL,
    tipo        VARCHAR(32)     NOT NULL,
    valor       FLOAT           NOT NULL,
    criado_em   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_alertas_esp_id (esp_id),
    KEY idx_alertas_tipo (tipo),
    CONSTRAINT fk_alertas_bracelete
        FOREIGN KEY (esp_id) REFERENCES braceletes (esp_id)
        ON UPDATE CASCADE
        ON DELETE RESTRICT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Pool of IDs the gateway can assign to slaves (em_uso=0).
INSERT INTO braceletes (esp_id, em_uso) VALUES
    (1, 0),
    (2, 0),
    (3, 0),
    (4, 0),
    (5, 0),
    (6, 0),
    (7, 0),
    (8, 0)
ON DUPLICATE KEY UPDATE esp_id = esp_id;

-- Optional: user for the ESP gateway (MySQL 8 needs native password).
-- CREATE USER IF NOT EXISTS 'savi'@'%' IDENTIFIED WITH mysql_native_password BY 'savi';
-- GRANT SELECT, INSERT, UPDATE ON savi.braceletes TO 'savi'@'%';
-- GRANT INSERT ON savi.alertas TO 'savi'@'%';
-- FLUSH PRIVILEGES;

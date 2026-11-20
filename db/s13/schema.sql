CREATE TABLE IF NOT EXISTS inspection_batches (
    batch_id VARBINARY(64) NOT NULL PRIMARY KEY,
    root_path TEXT NOT NULL,
    config_sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    state VARCHAR(9) CHARACTER SET ascii COLLATE ascii_bin NOT NULL
        CHECK (state IN ('running', 'completed', 'stopped')),
    total_files INT UNSIGNED NOT NULL CHECK (total_files > 0),
    created_at DATETIME(6) NOT NULL DEFAULT (UTC_TIMESTAMP(6)),
    INDEX inspection_batches_recent (created_at, batch_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;

CREATE TABLE IF NOT EXISTS inspection_batch_members (
    batch_id VARBINARY(64) NOT NULL,
    sequence_no INT UNSIGNED NOT NULL CHECK (sequence_no > 0),
    inspection_id VARBINARY(64) NOT NULL UNIQUE,
    PRIMARY KEY (batch_id, sequence_no),
    FOREIGN KEY (batch_id) REFERENCES inspection_batches (batch_id),
    FOREIGN KEY (inspection_id) REFERENCES inspections (inspection_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;

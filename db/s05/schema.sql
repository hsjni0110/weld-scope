CREATE TABLE IF NOT EXISTS inspections (
    inspection_id VARBINARY(64) NOT NULL PRIMARY KEY,
    request_id VARBINARY(64) NOT NULL UNIQUE,
    input_id VARCHAR(255) NOT NULL,
    source_path TEXT NOT NULL,
    source_sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    config_sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    algorithm_version VARCHAR(128) NOT NULL,
    dataset_version VARCHAR(128) NOT NULL,
    decision VARCHAR(8) NOT NULL CHECK (decision IN ('PASS', 'FAIL', 'REVIEW', 'ERROR')),
    reason TEXT NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT (UTC_TIMESTAMP(6)),
    CHECK (OCTET_LENGTH(inspection_id) > 0 AND OCTET_LENGTH(request_id) > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;

CREATE TABLE IF NOT EXISTS analysis_attempts (
    inspection_id VARBINARY(64) NOT NULL,
    sequence_no INT UNSIGNED NOT NULL CHECK (sequence_no > 0),
    stage VARCHAR(32) NOT NULL CHECK (CHAR_LENGTH(stage) > 0),
    score DOUBLE NULL,
    decision VARCHAR(8) NOT NULL CHECK (decision IN ('PASS', 'FAIL', 'REVIEW', 'ERROR')),
    reason TEXT NOT NULL,
    duration_ms DOUBLE NOT NULL CHECK (duration_ms >= 0),
    PRIMARY KEY (inspection_id, sequence_no),
    FOREIGN KEY (inspection_id) REFERENCES inspections (inspection_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;

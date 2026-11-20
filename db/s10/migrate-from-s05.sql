ALTER TABLE inspections
    MODIFY COLUMN source_sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    MODIFY COLUMN config_sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    MODIFY COLUMN decision VARCHAR(8) NULL,
    ADD COLUMN work_state VARCHAR(9) NOT NULL DEFAULT 'COMPLETED',
    ADD CONSTRAINT inspection_work_state CHECK (
        (work_state = 'CANCELLED' AND decision IS NULL) OR
        (work_state = 'COMPLETED' AND decision IS NOT NULL));

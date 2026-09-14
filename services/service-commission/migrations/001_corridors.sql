CREATE TABLE corridors (
    corridor_id    TEXT PRIMARY KEY,
    from_country   CHAR(2)       NOT NULL,
    to_country     CHAR(2)       NOT NULL,
    currency       CHAR(3)       NOT NULL,
    base_fee       NUMERIC(18,2) NOT NULL CHECK (base_fee >= 0),
    percentage_fee NUMERIC(8,5)  NOT NULL CHECK (percentage_fee >= 0 AND percentage_fee < 1),
    fixed_fee      NUMERIC(18,2) NOT NULL CHECK (fixed_fee >= 0),
    min_limit      NUMERIC(18,2) NOT NULL,
    max_limit      NUMERIC(18,2) NOT NULL,
    min_fee        NUMERIC(18,2) NOT NULL,
    max_fee        NUMERIC(18,2) NOT NULL,
    CHECK (from_country <> to_country),
    CHECK (min_limit > 0 AND min_limit <= max_limit),
    CHECK (min_fee >= 0 AND min_fee <= max_fee),
    UNIQUE (from_country, to_country, currency)
);

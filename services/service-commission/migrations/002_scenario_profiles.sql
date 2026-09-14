CREATE TABLE scenario_profiles (
    scenario       TEXT PRIMARY KEY
                   CHECK (scenario IN ('cbdc', 'bank_transfer', 'smart_contract', 'trade_finance')),
    title          TEXT         NOT NULL,
    description    TEXT         NOT NULL,
    fee_multiplier NUMERIC(6,3) NOT NULL CHECK (fee_multiplier > 0),
    eta_label      TEXT         NOT NULL,
    limitations    TEXT[]       NOT NULL DEFAULT '{}',
    min_amount     NUMERIC(18,2),
    max_amount     NUMERIC(18,2),
    countries      CHAR(2)[]
);

-- Домен сделки. Всё разделено по company_id: он приезжает в JWT (claim company_id)
-- и является единственной границей между арендаторами.
DROP TABLE IF EXISTS _placeholder;

-- Справочник документов: какие нужны и зачем (текст показывается пользователю).
CREATE TABLE document_kinds (
    kind         TEXT PRIMARY KEY,
    title        TEXT   NOT NULL,
    purpose      TEXT   NOT NULL,
    required_for TEXT[] NOT NULL
);

INSERT INTO document_kinds (kind, title, purpose, required_for) VALUES
    ('contract',   'Контракт',
     'Основание для валютного контроля: банк сверяет условия сделки с договором.',
     ARRAY['cbdc', 'bank_transfer', 'smart_contract', 'trade_finance']),
    ('invoice',    'Инвойс',
     'Подтверждает сумму и состав поставки.',
     ARRAY['cbdc', 'bank_transfer', 'smart_contract', 'trade_finance']),
    ('deal_passport', 'Паспорт сделки (УНК)',
     'Уникальный номер контракта: нужен банку для постановки сделки на учёт.',
     ARRAY['bank_transfer', 'trade_finance']),
    ('letter_of_credit', 'Заявление на аккредитив',
     'Поручение банку раскрыть аккредитив в пользу контрагента.',
     ARRAY['trade_finance']),
    ('digital_signature', 'Подтверждение ЭЦП сторон',
     'Смарт-контракт исполняется только при подписи обеих сторон.',
     ARRAY['smart_contract']);

CREATE TABLE deals (
    id                   UUID PRIMARY KEY,
    company_id           UUID          NOT NULL,
    display_no           BIGINT        GENERATED ALWAYS AS IDENTITY,
    counterparty_country CHAR(2)       NOT NULL,
    counterparty_name    TEXT          NOT NULL CHECK (length(trim(counterparty_name)) > 0),
    operation_type       TEXT          NOT NULL CHECK (operation_type IN ('import', 'export')),
    amount               NUMERIC(18,2) NOT NULL CHECK (amount > 0),
    currency             CHAR(3)       NOT NULL,
    scenario             TEXT          CHECK (scenario IN ('cbdc', 'bank_transfer', 'smart_contract', 'trade_finance')),
    commission_total     NUMERIC(18,2),
    commission_breakdown JSONB,
    stage                TEXT          NOT NULL
                         CHECK (stage IN ('created', 'documents', 'compliance_check', 'settlement', 'completed', 'blocked')),
    blocked_from         TEXT          CHECK (blocked_from IN ('documents', 'compliance_check', 'settlement')),
    blocker_reason       TEXT,
    next_action_at       TIMESTAMPTZ,  -- когда эмулятору трогать сделку; NULL = ждём пользователя
    version              INT           NOT NULL DEFAULT 0,
    created_at           TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at           TIMESTAMPTZ   NOT NULL DEFAULT now(),
    CHECK ((stage = 'blocked') = (blocked_from IS NOT NULL))
);

CREATE INDEX deals_company_idx ON deals(company_id, updated_at DESC);
CREATE INDEX deals_next_action_idx ON deals(next_action_at)
    WHERE next_action_at IS NOT NULL AND stage <> 'completed';

CREATE TABLE deal_documents (
    id             UUID PRIMARY KEY,
    deal_id        UUID        NOT NULL REFERENCES deals(id) ON DELETE CASCADE,
    kind           TEXT        NOT NULL REFERENCES document_kinds(kind),
    status         TEXT        NOT NULL
                   CHECK (status IN ('missing', 'uploaded', 'under_review', 'approved', 'rejected')),
    reject_reason  TEXT,
    next_action_at TIMESTAMPTZ,
    updated_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (deal_id, kind)
);

CREATE INDEX deal_documents_next_action_idx ON deal_documents(next_action_at)
    WHERE next_action_at IS NOT NULL;

CREATE TABLE timeline_events (
    id           UUID PRIMARY KEY,
    deal_id      UUID        NOT NULL REFERENCES deals(id) ON DELETE CASCADE,
    seq          INT         NOT NULL,
    step         TEXT        NOT NULL,
    actor        TEXT        NOT NULL,
    status       TEXT        NOT NULL CHECK (status IN ('pending', 'in_progress', 'done', 'delayed')),
    delay_reason TEXT,
    started_at   TIMESTAMPTZ,
    finished_at  TIMESTAMPTZ,
    UNIQUE (deal_id, seq)
);

CREATE TABLE notifications (
    id         UUID PRIMARY KEY,
    company_id UUID        NOT NULL,
    deal_id    UUID        REFERENCES deals(id) ON DELETE CASCADE,
    severity   TEXT        NOT NULL CHECK (severity IN ('info', 'warning', 'critical')),
    message    TEXT        NOT NULL,
    read_at    TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX notifications_company_idx ON notifications(company_id, created_at DESC);

-- Transactional outbox: событие пишется в той же транзакции, что и изменение,
-- поэтому не теряется и не опережает данные. Порядок доставки — по id.
CREATE TABLE outbox (
    id           BIGINT      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    company_id   UUID        NOT NULL,
    topic        TEXT        NOT NULL,
    payload      JSONB       NOT NULL,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    published_at TIMESTAMPTZ
);

CREATE INDEX outbox_unpublished_idx ON outbox(id) WHERE published_at IS NULL;

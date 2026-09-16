-- Пользователь работает от имени юрлица: сделки, документы и уведомления
-- в service-core разделены по company_id, который приезжает в JWT.
CREATE TABLE IF NOT EXISTS companies (
    id         UUID PRIMARY KEY,
    name       TEXT        NOT NULL CHECK (length(trim(name)) > 0),
    inn        TEXT        NOT NULL UNIQUE CHECK (inn ~ '^[0-9]{10}$'),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

ALTER TABLE users ADD COLUMN IF NOT EXISTS company_id UUID REFERENCES companies(id);

-- Существующие (тестовые) пользователи получают по собственной компании,
-- иначе колонку нельзя объявить NOT NULL.
WITH numbered AS (
    SELECT id,
           COALESCE(NULLIF(trim(name), ''), login) AS company_name,
           lpad(row_number() OVER (ORDER BY created_at, id)::text, 10, '0') AS inn
    FROM users
    WHERE company_id IS NULL
), created AS (
    INSERT INTO companies (id, name, inn)
    SELECT gen_random_uuid(), company_name, inn FROM numbered
    RETURNING id, inn
)
UPDATE users u
SET company_id = created.id
FROM numbered, created
WHERE u.id = numbered.id AND created.inn = numbered.inn;

ALTER TABLE users ALTER COLUMN company_id SET NOT NULL;

CREATE INDEX IF NOT EXISTS users_company_idx ON users(company_id);

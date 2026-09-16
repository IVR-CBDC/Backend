-- Счётчик попыток подачи документа: apply() инкрементирует его каждый раз,
-- когда mutation переводит документ в uploaded (см. repository.cc). Даёт
-- эмулятору (src/emulator.cc) честный источник attempt для
-- documentApproved(deal_id, kind, attempt) — переподача (attempt >= 1)
-- никогда не отклоняется повторно, иначе отклонённый документ был бы
-- невосстановим навсегда.
--
-- Additive и идемпотентно: применяется поверх уже накатанных 001+002 без
-- их изменения.
ALTER TABLE deal_documents ADD COLUMN IF NOT EXISTS submit_count INT NOT NULL DEFAULT 0;

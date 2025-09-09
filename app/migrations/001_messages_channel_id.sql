-- 001_messages_channel_id.sql
BEGIN;

-- 1) Add the new column (nullable first so we can backfill)
ALTER TABLE messages
    ADD COLUMN IF NOT EXISTS channel_id BIGINT;

-- 2) Backfill from the channel name (one-time)
UPDATE messages m
SET channel_id = c.id
    FROM channels c
WHERE m.channel = c.name
  AND m.channel_id IS NULL;

-- 3) Make it NOT NULL
ALTER TABLE messages
    ALTER COLUMN channel_id SET NOT NULL;

-- 4) Add FK (idempotent)
DO $$
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.table_constraints
    WHERE constraint_name = 'fk_messages_channel'
      AND table_name = 'messages'
  ) THEN
ALTER TABLE messages
    ADD CONSTRAINT fk_messages_channel
        FOREIGN KEY (channel_id) REFERENCES channels(id) ON DELETE CASCADE;
END IF;
END $$;

-- 5) Fast history lookups by channel
CREATE INDEX IF NOT EXISTS idx_messages_channel_id
    ON messages(channel_id, id DESC);

COMMIT;

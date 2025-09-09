BEGIN;

-- Enum for channels.kind
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_type WHERE typname='channel_kind') THEN
    CREATE TYPE channel_kind AS ENUM ('category','text','voice');
  END IF;
END $$;

-- Guilds + members
CREATE TABLE IF NOT EXISTS guilds (
  id          BIGSERIAL PRIMARY KEY,
  name        TEXT NOT NULL,
  created_by  BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE TABLE IF NOT EXISTS guild_members (
  guild_id    BIGINT NOT NULL REFERENCES guilds(id) ON DELETE CASCADE,
  user_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  nick        TEXT,
  joined_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (guild_id, user_id)
);

-- Alter legacy channels in-place (add cols if missing)
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_name='channels' AND column_name='guild_id') THEN
    ALTER TABLE channels ADD COLUMN guild_id BIGINT;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_name='channels' AND column_name='parent_id') THEN
    ALTER TABLE channels ADD COLUMN parent_id BIGINT;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_name='channels' AND column_name='kind') THEN
    ALTER TABLE channels ADD COLUMN kind channel_kind;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_name='channels' AND column_name='position') THEN
    ALTER TABLE channels ADD COLUMN position INT NOT NULL DEFAULT 0;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_name='channels' AND column_name='is_private') THEN
    ALTER TABLE channels ADD COLUMN is_private BOOLEAN NOT NULL DEFAULT FALSE;
  END IF;
END $$;

-- Default guild & membership for backfill
WITH owner AS (
  SELECT id, username FROM users ORDER BY id LIMIT 1
), g AS (
  INSERT INTO guilds(name, created_by)
  SELECT 'Default Guild', id FROM owner
  ON CONFLICT DO NOTHING
  RETURNING id, created_by
)
INSERT INTO guild_members(guild_id, user_id, nick)
SELECT (SELECT id FROM guilds WHERE name='Default Guild' ORDER BY id LIMIT 1), id, username
FROM owner
ON CONFLICT DO NOTHING;

-- Backfill existing channels
UPDATE channels
SET guild_id = (SELECT id FROM guilds WHERE name='Default Guild' ORDER BY id LIMIT 1)
WHERE guild_id IS NULL;

UPDATE channels
SET kind = 'text'
WHERE kind IS NULL;

-- FK + index
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname='fk_channels_guild') THEN
    ALTER TABLE channels
      ADD CONSTRAINT fk_channels_guild
      FOREIGN KEY (guild_id) REFERENCES guilds(id) ON DELETE CASCADE;
  END IF;
END $$;

CREATE INDEX IF NOT EXISTS idx_channels_guild ON channels(guild_id, position, id);

COMMIT;

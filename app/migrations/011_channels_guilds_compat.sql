-- 011_channels_guilds_compat.sql
BEGIN;

-- Ensure guilds and guild_members tables exist
CREATE TABLE IF NOT EXISTS guilds (
  id BIGSERIAL PRIMARY KEY,
  name TEXT NOT NULL,
  created_by BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS guild_members (
  guild_id BIGINT NOT NULL REFERENCES guilds(id) ON DELETE CASCADE,
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  nick TEXT,
  joined_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (guild_id, user_id)
);

-- Ensure channel_kind enum exists
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_type WHERE typname = 'channel_kind') THEN
    CREATE TYPE channel_kind AS ENUM ('category','text','voice');
  END IF;
END $$;

-- Alter legacy channels table to match guild-aware schema
ALTER TABLE channels ADD COLUMN IF NOT EXISTS guild_id BIGINT;
ALTER TABLE channels ADD COLUMN IF NOT EXISTS parent_id BIGINT REFERENCES channels(id) ON DELETE SET NULL;
ALTER TABLE channels ADD COLUMN IF NOT EXISTS kind channel_kind NOT NULL DEFAULT 'text';
ALTER TABLE channels ADD COLUMN IF NOT EXISTS position INT NOT NULL DEFAULT 0;
ALTER TABLE channels ADD COLUMN IF NOT EXISTS is_private BOOLEAN NOT NULL DEFAULT FALSE;

CREATE INDEX IF NOT EXISTS idx_channels_guild ON channels(guild_id, position, id);

-- Backfill existing channels into a default guild and seed membership
DO $$
DECLARE gid BIGINT;
BEGIN
  SELECT id INTO gid FROM guilds LIMIT 1;
  IF gid IS NULL THEN
    INSERT INTO guilds(name, created_by)
    SELECT 'Test Guild', id FROM users ORDER BY id LIMIT 1
    RETURNING id INTO gid;
  END IF;

  IF gid IS NOT NULL THEN
    INSERT INTO guild_members(guild_id, user_id)
      SELECT gid, id FROM users ON CONFLICT DO NOTHING;

    UPDATE channels
      SET guild_id = gid
      WHERE guild_id IS NULL;

    UPDATE messages m
      SET guild_id = gid
      FROM channels c
      WHERE m.channel_id = c.id AND m.guild_id IS NULL;
  END IF;
END $$;

COMMIT;

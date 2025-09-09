-- Seed 'Secret Fishing Club' only when a user exists
DO $$
DECLARE u_id BIGINT;
BEGIN
  SELECT id INTO u_id FROM users ORDER BY id LIMIT 1;
  IF u_id IS NULL THEN
    RETURN;
  END IF;

  INSERT INTO guilds(name, created_by)
  VALUES ('Secret Fishing Club', u_id)
  ON CONFLICT DO NOTHING;

  INSERT INTO guild_members(guild_id, user_id, nick)
  SELECT g.id, u_id, u.username
  FROM guilds g
  JOIN users u ON u.id = u_id
  WHERE g.name='Secret Fishing Club'
  ON CONFLICT DO NOTHING;

  WITH gid AS (SELECT id FROM guilds WHERE name='Secret Fishing Club' ORDER BY id DESC LIMIT 1),
  cat_text AS (
    INSERT INTO channels(guild_id, kind, name, position, is_private)
    SELECT (SELECT id FROM gid), 'category', 'Text Channels', 0, FALSE
    ON CONFLICT DO NOTHING RETURNING id
  ),
  cat_voice AS (
    INSERT INTO channels(guild_id, kind, name, position, is_private)
    SELECT (SELECT id FROM gid), 'category', 'Voice Channels', 100, FALSE
    ON CONFLICT DO NOTHING RETURNING id
  )
  INSERT INTO channels(guild_id, parent_id, kind, name, position, is_private)
  VALUES
  ((SELECT id FROM gid),(SELECT id FROM cat_text),'text','fish-chat',10,FALSE),
  ((SELECT id FROM gid),(SELECT id FROM cat_text),'text','clips-and-highlights',20,FALSE),
  ((SELECT id FROM gid),(SELECT id FROM cat_voice),'voice','Gaming',110,FALSE)
  ON CONFLICT DO NOTHING;
END $$;

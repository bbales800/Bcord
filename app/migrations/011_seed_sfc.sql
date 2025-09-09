BEGIN;
WITH owner AS (
  SELECT id, username FROM users ORDER BY id LIMIT 1
),
g AS (
  INSERT INTO guilds(name, created_by)
  SELECT 'Secret Fishing Club', id FROM owner
  ON CONFLICT DO NOTHING
  RETURNING id, created_by
)
INSERT INTO guild_members(guild_id, user_id, nick)
SELECT (SELECT id FROM guilds WHERE name='Secret Fishing Club' ORDER BY id LIMIT 1),
       (SELECT id FROM owner), (SELECT username FROM owner)
ON CONFLICT DO NOTHING;

WITH gid AS (SELECT id FROM guilds WHERE name='Secret Fishing Club' ORDER BY id DESC LIMIT 1),
cat_text AS (
  INSERT INTO channels(guild_id, kind, name, position, is_private)
  SELECT (SELECT id FROM gid), 'category', 'Text Channels', 0, FALSE
  ON CONFLICT DO NOTHING
  RETURNING id
),
cat_voice AS (
  INSERT INTO channels(guild_id, kind, name, position, is_private)
  SELECT (SELECT id FROM gid), 'category', 'Voice Channels', 100, FALSE
  ON CONFLICT DO NOTHING
  RETURNING id
)
INSERT INTO channels(guild_id, parent_id, kind, name, position, is_private)
VALUES
((SELECT id FROM gid),(SELECT id FROM cat_text),'text','fish-chat',10,FALSE),
((SELECT id FROM gid),(SELECT id FROM cat_text),'text','clips-and-highlights',20,FALSE),
((SELECT id FROM gid),(SELECT id FROM cat_voice),'voice','Gaming',110,FALSE)
ON CONFLICT DO NOTHING;
COMMIT;

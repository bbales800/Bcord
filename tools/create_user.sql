\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS pgcrypto;

\echo '--- BCord New User Wizard ---'
\prompt 'Email: ' email
\prompt 'Username: ' username
\prompt 'Plain password (will be bcrypt-hashed): ' password

-- Upsert user and return id
WITH upsert AS (
  INSERT INTO users (email, username, pw_hash)
  VALUES (:'email', :'username', crypt(:'password', gen_salt('bf',12)))
  ON CONFLICT (username) DO UPDATE SET email = EXCLUDED.email
  RETURNING id
), uid AS (
  SELECT id FROM upsert
  UNION ALL
  SELECT id FROM users WHERE username = :'username' LIMIT 1
), tok AS (
  SELECT encode(gen_random_bytes(24),'hex') AS token
)
INSERT INTO sessions(token, user_id, expires_at)
SELECT tok.token, uid.id, now() + interval '30 days'
FROM tok, uid
RETURNING
  (SELECT id FROM uid LIMIT 1)      AS user_id,
  token                              AS session_token,
  now() + interval '30 days'         AS expires_at;

\echo 'User created/updated and session token issued above.'
\echo 'Paste session_token into your tester''s "Auth token" field.'

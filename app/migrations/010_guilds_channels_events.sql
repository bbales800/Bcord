BEGIN;

-- ===== 1) Guilds (aka servers) =======================================================
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

-- Optional: invites later

-- ===== 2) Roles & permissions (simple bitmask to start) ===============================
CREATE TABLE IF NOT EXISTS roles (
                                     id         BIGSERIAL PRIMARY KEY,
                                     guild_id   BIGINT NOT NULL REFERENCES guilds(id) ON DELETE CASCADE,
    name       TEXT NOT NULL,
    perm_bits  BIGINT NOT NULL DEFAULT 0,   -- e.g., 1<<0 send, 1<<1 manage, etc.
    UNIQUE (guild_id, name)
    );

CREATE TABLE IF NOT EXISTS member_roles (
                                            guild_id   BIGINT NOT NULL,
                                            user_id    BIGINT NOT NULL,
                                            role_id    BIGINT NOT NULL REFERENCES roles(id) ON DELETE CASCADE,
    PRIMARY KEY (guild_id, user_id, role_id),
    FOREIGN KEY (guild_id, user_id) REFERENCES guild_members(guild_id, user_id) ON DELETE CASCADE
    );

-- ===== 3) Channels (category, text, voice) ===========================================
CREATE TYPE channel_kind AS ENUM ('category','text','voice');

CREATE TABLE IF NOT EXISTS channels (
                                        id          BIGSERIAL PRIMARY KEY,
                                        guild_id    BIGINT NOT NULL REFERENCES guilds(id) ON DELETE CASCADE,
    parent_id   BIGINT REFERENCES channels(id) ON DELETE SET NULL, -- category container
    kind        channel_kind NOT NULL,
    name        TEXT NOT NULL,
    position    INT  NOT NULL DEFAULT 0,
    is_private  BOOLEAN NOT NULL DEFAULT FALSE
    );

-- Private channel ACL (optional now, nice to have)
CREATE TABLE IF NOT EXISTS channel_memberships (
                                                   channel_id  BIGINT NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
    user_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    PRIMARY KEY (channel_id, user_id)
    );

CREATE INDEX IF NOT EXISTS idx_channels_guild ON channels(guild_id, position, id);

-- ===== 4) Messages already exist; add guild_id & foreign key for fast multi-guild ====
ALTER TABLE messages ADD COLUMN IF NOT EXISTS guild_id BIGINT;
UPDATE messages m
SET guild_id = c.gid
    FROM (
  SELECT ch.id AS ch_id, ch.guild_id AS gid FROM channels ch
) c
WHERE m.guild_id IS NULL
  AND m.channel_id = c.ch_id;

-- If no mapping yet (legacy), fill with a default guild later after you create it.

CREATE INDEX IF NOT EXISTS idx_messages_guild_chan ON messages(guild_id, channel_id, id DESC);

-- ===== 5) Events =====================================================================
CREATE TABLE IF NOT EXISTS events (
                                      id          BIGSERIAL PRIMARY KEY,
                                      guild_id    BIGINT NOT NULL REFERENCES guilds(id) ON DELETE CASCADE,
    created_by  BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title       TEXT NOT NULL,
    description TEXT,
    starts_at   TIMESTAMPTZ NOT NULL,
    ends_at     TIMESTAMPTZ,
    location    TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
    );

CREATE TABLE IF NOT EXISTS event_rsvps (
                                           event_id    BIGINT NOT NULL REFERENCES events(id) ON DELETE CASCADE,
    user_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    status      TEXT NOT NULL CHECK (status IN ('going','interested','declined')),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (event_id, user_id)
    );

COMMIT;

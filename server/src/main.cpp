// =====================================================================================
// BCORD SERVER (Boost.Beast WebSocket) - Durable History by channel_id +
// Presence
// =====================================================================================
//
// WHAT THIS FILE DOES (clean version):
//  - Persists messages by channel_id (Postgres).
//  - History endpoints (by channel_id only):
//      * history         -> latest page (DESC on server; client may reverse)
//      * history_before  -> older page before <id> (DESC)
//      * history_since   -> delta newer than <id> (ASC)
//  - Live message broadcast includes "message_id" (DB row id).
//  - Presence via Redis (join/leave), lobby via channels_list.
//
// Where to edit later (search banners):
//   ===== CONFIG TUNABLES =====
//   ===== PG SCHEMA & PREPARES =====
//   ===== HISTORY TYPES & HELPERS =====
//   ===== WS HANDLERS =====
//
// =====================================================================================

// =====================================================================================
// BCORD SERVER (Boost.Beast WebSocket) - Durable History by channel_id +
// Guilds/Channels
// =====================================================================================

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lan_mac.hpp"
#include <argon2.h>
// Fix libpqxx ABI mismatch on Ubuntu 24.04 by hiding std::source_location symbols
#ifndef PQXX_HIDE_SOURCE_LOCATION
#define PQXX_HIDE_SOURCE_LOCATION
#endif
#include <pqxx/pqxx>
#include <sw/redis++/redis++.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

// ===== CONFIG
// ========================================================================
static constexpr int SESSION_HOURS_DEFAULT = 24 * 30; // 30 days
static constexpr int HISTORY_PAGE_DEFAULT = 100;
static constexpr int HISTORY_PAGE_MAX = 500;
static constexpr int ONLINE_TTL_SECONDS = 90;

// ===== GLOBALS
// =======================================================================
struct RedisEnv {
  std::unique_ptr<sw::redis::Redis> conn;
  bool enabled = false;
} g_redis;
struct PgEnv {
  std::unique_ptr<pqxx::connection> conn;
  bool enabled = false;
} g_pg;

static std::string now_stamp() {
  using clock = std::chrono::system_clock;
  auto t = clock::to_time_t(clock::now());
  std::tm tm{};
  localtime_r(&t, &tm);
  std::ostringstream os;
  os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return os.str();
}
static std::string make_session_id() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static const char *hex = "0123456789abcdef";
  std::string s(16, '0');
  for (auto &c : s)
    c = hex[rng() & 0xF];
  return s;
}
static std::string random_token(size_t n = 48) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static const char *base =
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::string s;
  s.reserve(n);
  for (size_t i = 0; i < n; i++)
    s.push_back(base[rng() % 62]);
  return s;
}
static std::string chkey(long id) { return "chan#" + std::to_string(id); }

// ===== FWD DECLS
// =====================================================================
struct Session;
struct ChannelRegistry {
  std::unordered_map<std::string, std::unordered_set<Session *>> map;
  mutable std::mutex mu;
  void join(const std::string &ch, Session *s);
  void leave(const std::string &ch, Session *s);
  std::vector<std::pair<long, std::string>>
  users_in(const std::string &ch) const;
  void broadcast(const std::string &ch, const json &msg);
};
static ChannelRegistry g_channels;

// ===== POSTGRES INIT (auth / channels / messages)
// ====================================
static void pg_init_auth_schema() {
  if (!g_pg.enabled)
    return;
  pqxx::work tx{*g_pg.conn};
  tx.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS users(
          id          BIGSERIAL PRIMARY KEY,
          email       TEXT UNIQUE NOT NULL,
          username    TEXT UNIQUE NOT NULL,
          pw_hash     TEXT NOT NULL,
          created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
        CREATE TABLE IF NOT EXISTS sessions(
          token       TEXT PRIMARY KEY,
          user_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
          expires_at  TIMESTAMPTZ NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);
    )SQL");
  tx.commit();

  g_pg.conn->prepare(
      "user_by_email",
      "SELECT id,email,username,pw_hash FROM users WHERE email=$1");
  g_pg.conn->prepare(
      "user_by_name",
      "SELECT id,email,username,pw_hash FROM users WHERE username=$1");
  g_pg.conn->prepare("user_by_id", "SELECT username FROM users WHERE id=$1");
  g_pg.conn->prepare("user_insert", "INSERT INTO users(email,username,pw_hash) "
                                    "VALUES($1,$2,$3) RETURNING id");

  g_pg.conn->prepare(
      "session_select_valid",
      "SELECT user_id FROM sessions WHERE token=$1 AND expires_at > now()");
  g_pg.conn->prepare("session_insert_hours",
                     "INSERT INTO sessions(token,user_id,expires_at) "
                     "VALUES($1,$2, now() + make_interval(hours => $3::int))");
  g_pg.conn->prepare("session_delete", "DELETE FROM sessions WHERE token=$1");
}

static void pg_init_channel_schema() {
  if (!g_pg.enabled)
    return;

  // Existing prepares you may already have for channel auth - keep as needed.
  g_pg.conn->prepare("channel_is_joinable",
                     "SELECT true AS ok"); // allow all for now
  g_pg.conn->prepare("channel_name_by_id",
                     "SELECT name FROM channels WHERE id=$1");
  g_pg.conn->prepare("channel_id_by_name",
                     "SELECT id FROM channels WHERE name=$1");
  g_pg.conn->prepare("subscribe_upsert",
                     "INSERT INTO channel_memberships(channel_id,user_id) "
                     "VALUES($1,$2) ON CONFLICT DO NOTHING");
  g_pg.conn->prepare(
      "unsubscribe_update",
      "DELETE FROM channel_memberships WHERE channel_id=$1 AND user_id=$2");

  // ====== NEW: guilds/channels prepares (inside function!)
  // ==========================
  g_pg.conn->prepare("guilds_for_user",
                     "SELECT g.id, g.name "
                     "FROM guilds g JOIN guild_members m ON m.guild_id=g.id "
                     "WHERE m.user_id=$1 ORDER BY g.name");

  g_pg.conn->prepare("channels_for_guild",
                     "SELECT id, guild_id, parent_id, kind::text AS kind, "
                     "name, position, is_private "
                     "FROM channels WHERE guild_id=$1 ORDER BY position, id");

  g_pg.conn->prepare(
      "guild_insert",
      "INSERT INTO guilds(name,created_by) VALUES($1,$2) RETURNING id");

  g_pg.conn->prepare("guild_member_insert",
                     "INSERT INTO guild_members(guild_id,user_id,nick) "
                     "VALUES($1,$2,$3) ON CONFLICT DO NOTHING");

  // pass parent_id=0 from C++; SQL turns it into NULL
  g_pg.conn->prepare(
      "channel_insert",
      "INSERT INTO channels(guild_id,parent_id,kind,name,position,is_private) "
      "VALUES($1, NULLIF($2,0), $3::channel_kind, $4, $5, $6) RETURNING id");

  // --- PHASE 2 prepares: channel info / rename / delete / move ---
  g_pg.conn->prepare("channel_info",
                     "SELECT guild_id, parent_id, kind::text AS kind, "
                     "is_private FROM channels WHERE id=$1");

  g_pg.conn->prepare(
      "channel_update_name",
      "UPDATE channels SET name=$2 WHERE id=$1 RETURNING id, name");

  g_pg.conn->prepare("channel_delete", "DELETE FROM channels WHERE id=$1");

  g_pg.conn->prepare(
      "channel_move",
      "UPDATE channels SET parent_id = NULLIF($2,0), position=$3 WHERE id=$1 "
      "RETURNING id, parent_id, position");

  // Joinability: allowed if channel is NOT private or user has membership
  g_pg.conn->prepare(
      "channel_joinable_check",
      "SELECT (NOT c.is_private) OR EXISTS (SELECT 1 FROM channel_memberships "
      "m WHERE m.channel_id=c.id AND m.user_id=$2) AS ok "
      "FROM channels c WHERE c.id=$1");
}

// ----- messages schema + prepares (by channel_id) ------------------------------------
static void pg_init_msg_schema() {
    if (!g_pg.enabled) return;
    pqxx::work tx{*g_pg.conn};
    tx.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS messages(
          id          BIGSERIAL PRIMARY KEY,
          guild_id    BIGINT,
          channel_id  BIGINT,
          username    TEXT   NOT NULL,
          body        TEXT   NOT NULL,
          ts          BIGINT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_messages_channel_id ON messages(channel_id, id DESC);
        CREATE INDEX IF NOT EXISTS idx_messages_guild_chan ON messages(guild_id, channel_id, id DESC);
    )SQL");
    tx.commit();

    // Insert by ids
    g_pg.conn->prepare("insert_msg_id",
        "INSERT INTO messages(guild_id,channel_id,username,body,ts) VALUES($1,$2,$3,$4,$5) RETURNING id");

    // History pages
    g_pg.conn->prepare("hist_latest",
        "SELECT id AS message_id, username AS user, body AS text, ts "
        "FROM messages WHERE channel_id=$1 ORDER BY id DESC LIMIT $2");

    g_pg.conn->prepare("hist_before",
        "SELECT id AS message_id, username AS user, body AS text, ts "
        "FROM messages WHERE channel_id=$1 AND id < $2 ORDER BY id DESC LIMIT $3");

    g_pg.conn->prepare("hist_since",
        "SELECT id AS message_id, username AS user, body AS text, ts "
        "FROM messages WHERE channel_id=$1 AND id > $2 ORDER BY id ASC LIMIT $3");
}

// ===== PG HELPERS
// ====================================================================
static std::optional<std::tuple<long, std::string, std::string, std::string>>
pg_find_user_by_email(const std::string &email) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("user_by_email", email);
  if (r.empty())
    return std::nullopt;
  return std::make_tuple(r[0]["id"].as<long>(), r[0]["email"].c_str(),
                         r[0]["username"].c_str(), r[0]["pw_hash"].c_str());
}
static std::optional<std::tuple<long, std::string, std::string, std::string>>
pg_find_user_by_name(const std::string &u) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("user_by_name", u);
  if (r.empty())
    return std::nullopt;
  return std::make_tuple(r[0]["id"].as<long>(), r[0]["email"].c_str(),
                         r[0]["username"].c_str(), r[0]["pw_hash"].c_str());
}
static std::optional<std::string> pg_username_by_id(long uid) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("user_by_id", uid);
  if (r.empty())
    return std::nullopt;
  return std::string(r[0]["username"].c_str());
}
static long pg_insert_user(const std::string &email, const std::string &user,
                           const std::string &hash) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("user_insert", email, user, hash);
  long id = r[0]["id"].as<long>();
  tx.commit();
  return id;
}
static std::string pg_create_session(long uid,
                                     int hours = SESSION_HOURS_DEFAULT) {
  std::string token = random_token();
  pqxx::work tx{*g_pg.conn};
  tx.exec_prepared("session_insert_hours", token, uid, hours);
  tx.commit();
  return token;
}
static std::optional<long> pg_validate_session(const std::string &token) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("session_select_valid", token);
  if (r.empty())
    return std::nullopt;
  return r[0]["user_id"].as<long>();
}
static long pg_channel_id_by_name(const std::string &name) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("channel_id_by_name", name);
  if (r.empty())
    return 0;
  return r[0]["id"].as<long>();
}
static std::optional<std::string> pg_channel_name_by_id(long id) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("channel_name_by_id", id);
  if (r.empty())
    return std::nullopt;
  return std::string(r[0]["name"].c_str());
}
static void pg_subscribe(long chan_id, long uid, bool on) {
  pqxx::work tx{*g_pg.conn};
  if (on)
    tx.exec_prepared("subscribe_upsert", chan_id, uid);
  else
    tx.exec_prepared("unsubscribe_update", chan_id, uid);
  tx.commit();
}
static bool pg_channel_is_joinable(long chan_id, long uid) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("channel_joinable_check", chan_id, uid);
  return !r.empty() && r[0]["ok"].as<bool>();
}

// --- PHASE 2 helpers: rename / delete / move --------------------------------
static std::optional<std::tuple<long, long, std::string, bool>>
pg_channel_fetch_info(long cid) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("channel_info", cid);
  if (r.empty())
    return std::nullopt;
  long gid = r[0]["guild_id"].is_null() ? 0 : r[0]["guild_id"].as<long>();
  long parent = r[0]["parent_id"].is_null() ? 0 : r[0]["parent_id"].as<long>();
  std::string kind = r[0]["kind"].c_str();
  bool priv = r[0]["is_private"].as<bool>();
  tx.commit();
  return std::make_tuple(gid, parent, kind, priv);
}

static std::optional<std::pair<long, std::string>>
pg_channel_rename(long cid, const std::string &name) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("channel_update_name", cid, name);
  if (r.empty())
    return std::nullopt;
  auto out = std::make_pair(r[0]["id"].as<long>(), r[0]["name"].c_str());
  tx.commit();
  return out;
}

static bool pg_channel_delete(long cid) {
  pqxx::work tx{*g_pg.conn};
  tx.exec_prepared("channel_delete", cid);
  tx.commit();
  return true;
}

static std::optional<std::tuple<long, long, int>>
pg_channel_move(long cid, std::optional<long> parent, int position) {
  pqxx::work tx{*g_pg.conn};
  auto r =
      tx.exec_prepared("channel_move", cid, parent ? *parent : 0, position);
  if (r.empty())
    return std::nullopt;
  long pid = r[0]["parent_id"].is_null() ? 0 : r[0]["parent_id"].as<long>();
  int pos = r[0]["position"].as<int>();
  auto out = std::make_tuple(r[0]["id"].as<long>(), pid, pos);
  tx.commit();
  return out;
}

// ===== GUILDS/CHANNELS HELPERS
// =======================================================
static json pg_guilds_for_user(long uid) {
  json arr = json::array();
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("guilds_for_user", uid);
  for (auto const &row : r) {
    json g;
    g["id"] = row["id"].as<long>();
    g["name"] = row["name"].c_str();
    arr.push_back(g);
  }
  return arr;
}
static json pg_channels_for_guild(long gid) {
  json cats = json::array(), text = json::array(), voice = json::array();
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("channels_for_guild", gid);
  for (auto const &row : r) {
    json c;
    c["id"] = row["id"].as<long>();
    c["guild_id"] = row["guild_id"].as<long>();
    if (!row["parent_id"].is_null())
      c["parent_id"] = row["parent_id"].as<long>();
    c["kind"] = row["kind"].c_str();
    c["name"] = row["name"].c_str();
    c["position"] = row["position"].as<int>();
    c["is_private"] = row["is_private"].as<bool>();
    std::string kind = row["kind"].c_str();
    if (kind == "category")
      cats.push_back(c);
    else if (kind == "text")
      text.push_back(c);
    else
      voice.push_back(c);
  }
  json out;
  out["categories"] = cats;
  out["text"] = text;
  out["voice"] = voice;
  return out;
}
static long pg_guild_create(const std::string &name, long owner,
                            const std::string &nick) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("guild_insert", name, owner);
  long gid = r[0]["id"].as<long>();
  tx.exec_prepared("guild_member_insert", gid, owner, nick);
  tx.commit();
  return gid;
}

// Optional: auto-seed a default guild if user has none
static void pg_guilds_ensure_default_for_user(long uid,
                                             const std::string &nick) {
  auto list = pg_guilds_for_user(uid);
  if (!list.empty())
    return;
  (void)pg_guild_create("Default Guild", uid, nick);
}
static long pg_channel_create(long gid, std::optional<long> parent,
                              const std::string &kind, const std::string &name,
                              int position, bool is_private = false) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("channel_insert", gid,
                            parent ? *parent : 0, // 0 -> NULLIF($2,0) in SQL
                            kind, name, position, is_private);
  long id = r[0]["id"].as<long>();
  tx.commit();
  return id;
}

// ===== REDIS PRESENCE
// ================================================================
static void redis_set_online(long uid) {
  if (!g_redis.enabled)
    return;
  try {
    g_redis.conn->setex("online:user:" + std::to_string(uid),
                        ONLINE_TTL_SECONDS, "1");
  } catch (...) {
  }
}
static void redis_room_add(long chan_id, long uid) {
  if (!g_redis.enabled)
    return;
  try {
    g_redis.conn->sadd("presence:chan:" + std::to_string(chan_id),
                       std::to_string(uid));
  } catch (...) {
  }
}
static void redis_room_del(long chan_id, long uid) {
  if (!g_redis.enabled)
    return;
  try {
    g_redis.conn->srem("presence:chan:" + std::to_string(chan_id),
                       std::to_string(uid));
  } catch (...) {
  }
}
static long redis_room_count(long chan_id) {
  if (!g_redis.enabled)
    return -1;
  try {
    return g_redis.conn->scard("presence:chan:" + std::to_string(chan_id));
  } catch (...) {
    return -1;
  }
}
static std::vector<long> redis_room_members(long chan_id) {
  std::vector<long> out;
  if (!g_redis.enabled)
    return out;
  try {
    std::vector<std::string> members;
    g_redis.conn->smembers("presence:chan:" + std::to_string(chan_id),
                           std::back_inserter(members));
    out.reserve(members.size());
    for (const auto &s : members) {
      try {
        out.push_back(std::stol(s));
      } catch (...) {
      }
    }
  } catch (...) {
  }
  return out;
}

// ===== HISTORY TYPES & HELPERS =======================================================
struct RowMsg { long long message_id; std::string user; std::string text; long long ts; };

static std::vector<RowMsg> pg_hist_latest(long cid, int limit) {
    std::vector<RowMsg> out; pqxx::work tx{*g_pg.conn};
    auto r = tx.exec_prepared("hist_latest", cid, std::max(1,std::min(limit, HISTORY_PAGE_MAX)));
    out.reserve(r.size());
    for (auto const& row : r)
        out.push_back({ row["message_id"].as<long long>(),
                        row["user"].c_str(),
                        row["text"].c_str(),
                        row["ts"].as<long long>() });
    return out;
}

static std::vector<RowMsg> pg_hist_before(long cid, long long before_id, int limit) {
    std::vector<RowMsg> out; pqxx::work tx{*g_pg.conn};
    auto r = tx.exec_prepared("hist_before", cid, before_id, std::max(1,std::min(limit, HISTORY_PAGE_MAX)));
    out.reserve(r.size());
    for (auto const& row : r)
        out.push_back({ row["message_id"].as<long long>(),
                        row["user"].c_str(),
                        row["text"].c_str(),
                        row["ts"].as<long long>() });
    return out;
}

static std::vector<RowMsg> pg_hist_since(long cid, long long after_id, int limit) {
    std::vector<RowMsg> out; pqxx::work tx{*g_pg.conn};
    auto r = tx.exec_prepared("hist_since", cid, after_id, std::max(1,std::min(limit, HISTORY_PAGE_MAX)));
    out.reserve(r.size());
    for (auto const& row : r)
        out.push_back({ row["message_id"].as<long long>(),
                        row["user"].c_str(),
                        row["text"].c_str(),
                        row["ts"].as<long long>() });
    return out;
}

// ===== MESSAGES
// ======================================================================
static long long pg_insert_message_id(long gid, long cid,
                                      const std::string &user,
                                      const std::string &body, long long ts) {
  pqxx::work tx{*g_pg.conn};
  auto r = tx.exec_prepared("insert_msg_id", gid, cid, user, body, ts);
  long long id = r[0]["id"].as<long long>();
  tx.commit();
  std::cerr << "[msg] inserted message_id=" << id << " cid=" << cid
            << " user=" << user << "\n";
  return id;
}

// ===== SESSION / CHANNEL REGISTRY
// ====================================================
struct Session {
  websocket::stream<tcp::socket> ws;
  std::string sid = make_session_id();
  std::string nick = "anon";
  bool authenticated = false;
  long user_id = 0;
  std::unordered_set<std::string> joined; // chkeys
  net::ip::address remote_addr{};
  unsigned short remote_port{};
  std::mutex write_mu;

  explicit Session(tcp::socket sock) : ws(std::move(sock)) {
    auto ep = ws.next_layer().remote_endpoint();
    remote_addr = ep.address();
    remote_port = ep.port();
  }
  void send(const json &j) {
    std::scoped_lock lk(write_mu);
    auto s = j.dump();
    ws.text(true);
    ws.write(net::buffer(s));
  }
  void join_key(const std::string &key) {
    if (joined.insert(key).second)
      g_channels.join(key, this);
  }
  void leave_key(const std::string &key) {
    if (joined.erase(key))
      g_channels.leave(key, this);
  }
  void leave_all() {
    for (const auto &key : joined) {
      if (key.rfind("chan#", 0) == 0) {
        long id = std::stol(key.substr(5));
        redis_room_del(id, user_id);
        g_channels.broadcast(key, {{"op", "left"},
                                   {"channel_id", id},
                                   {"user", nick},
                                   {"user_id", user_id}});
        json users = json::array();
        for (auto uid : redis_room_members(id))
          if (auto u = pg_username_by_id(uid))
            users.push_back({{"user_id", uid}, {"nick", *u}});
        g_channels.broadcast(
            key,
            {{"op", "presence_list_ok"}, {"channel_id", id}, {"users", users}});
      }
      g_channels.leave(key, this);
    }
    joined.clear();
  }
};

void ChannelRegistry::join(const std::string &ch, Session *s) {
  std::scoped_lock lk(mu);
  map[ch].insert(s);
}
void ChannelRegistry::leave(const std::string &ch, Session *s) {
  std::scoped_lock lk(mu);
  auto it = map.find(ch);
  if (it != map.end()) {
    it->second.erase(s);
    if (it->second.empty())
      map.erase(it);
  }
}
std::vector<std::pair<long, std::string>>
ChannelRegistry::users_in(const std::string &ch) const {
  std::vector<std::pair<long, std::string>> out;
  std::scoped_lock lk(mu);
  auto it = map.find(ch);
  if (it == map.end())
    return out;
  out.reserve(it->second.size());
  for (auto *s : it->second)
    out.push_back({s->user_id, s->nick});
  return out;
}
void ChannelRegistry::broadcast(const std::string &ch, const json &msg) {
  std::vector<Session *> targets;
  {
    std::scoped_lock lk(mu);
    auto it = map.find(ch);
    if (it == map.end())
      return;
    targets.reserve(it->second.size());
    for (auto *s : it->second)
      targets.push_back(s);
  }
  for (auto *s : targets) {
    try {
      s->send(msg);
    } catch (...) {
    }
  }
}
static void broadcast_roster_snapshot(long chan_id) {
  auto key = chkey(chan_id);
  json users = json::array();
  for (auto uid : redis_room_members(chan_id))
    if (auto u = pg_username_by_id(uid))
      users.push_back({{"user_id", uid}, {"nick", *u}});
  g_channels.broadcast(
      key,
      {{"op", "presence_list_ok"}, {"channel_id", chan_id}, {"users", users}});
}

// ===== SOCKET LOOP
// ===================================================================
void session_loop(tcp::socket socket) {
  auto sp = std::make_shared<Session>(std::move(socket));
  const auto who =
      sp->remote_addr.to_string() + ":" + std::to_string(sp->remote_port);
  std::cerr << "[" << now_stamp() << "] connect " << who << " sid=" << sp->sid
            << "\n";

  if (auto mac = lookup_mac_for_ip(sp->remote_addr.to_string()); !mac.empty())
    std::cerr << "[MAC] " << sp->remote_addr.to_string() << " -> " << mac
              << "\n";

  try {
    sp->ws.accept();
    sp->send({{"op", "welcome"}, {"session_id", sp->sid}});

    beast::flat_buffer buffer;
    while (true) {
      sp->ws.read(buffer);
      auto text = beast::buffers_to_string(buffer.data());
      buffer.consume(buffer.size());

      json in;
      try {
        in = json::parse(text);
      } catch (...) {
        sp->send({{"op", "error"}, {"reason", "invalid_json"}});
        continue;
      }

      const auto op = in.value("op", "");

      // -------- REGISTER --------
      if (op == "register") {
        if (!g_pg.enabled) {
          sp->send({{"op", "error"}, {"reason", "pg_disabled"}});
          continue;
        }
        std::string email = in.value("email", ""),
                    user = in.value("username", ""),
                    pw = in.value("password", "");
        if (email.empty() || user.empty() || pw.empty()) {
          sp->send({{"op", "error"}, {"reason", "missing_fields"}});
          continue;
        }
        if (pg_find_user_by_email(email) || pg_find_user_by_name(user)) {
          sp->send({{"op", "error"}, {"reason", "user_exists"}});
          continue;
        }
        uint32_t t_cost = 2, m_cost = 1 << 16, parallelism = 1;
        std::array<uint8_t, 16> salt{};
        for (auto &b : salt)
          b = uint8_t(std::rand() & 0xFF);
        char enc[argon2_encodedlen(t_cost, m_cost, parallelism, salt.size(), 32,
                                   Argon2_id)];
        if (argon2id_hash_encoded(t_cost, m_cost, parallelism, pw.data(),
                                  pw.size(), salt.data(), salt.size(), 32, enc,
                                  sizeof(enc)) != ARGON2_OK) {
          sp->send({{"op", "error"}, {"reason", "hash_failed"}});
          continue;
        }
        long uid = pg_insert_user(email, user, enc);
        std::string token = pg_create_session(uid);
        sp->authenticated = true;
        sp->user_id = uid;
        sp->nick = user;
        sp->send({{"op", "register_ok"},
                  {"token", token},
                  {"user_id", uid},
                  {"nick", sp->nick}});
        sp->send({{"op", "auth_ok"}, {"user_id", uid}, {"nick", sp->nick}});
      }
      // -------- LOGIN --------
      else if (op == "login") {
        if (!g_pg.enabled) {
          sp->send({{"op", "error"}, {"reason", "pg_disabled"}});
          continue;
        }
        std::string ident =
            in.value("id", in.value("username", in.value("email", "")));
        std::string pw = in.value("password", "");
        if (ident.empty() || pw.empty()) {
          sp->send({{"op", "error"}, {"reason", "missing_fields"}});
          continue;
        }
        auto u = (ident.find('@') != std::string::npos)
                     ? pg_find_user_by_email(ident)
                     : pg_find_user_by_name(ident);
        if (!u) {
          sp->send({{"op", "error"}, {"reason", "not_found"}});
          continue;
        }
        auto [uid, email, uname, pwhash] = *u;
        if (argon2id_verify(pwhash.c_str(), pw.data(), pw.size()) !=
            ARGON2_OK) {
          sp->send({{"op", "error"}, {"reason", "bad_password"}});
          continue;
        }
        std::string token = pg_create_session(uid);
        sp->authenticated = true;
        sp->user_id = uid;
        sp->nick = uname;
        sp->send({{"op", "login_ok"},
                  {"token", token},
                  {"user_id", uid},
                  {"nick", sp->nick}});
        sp->send({{"op", "auth_ok"}, {"user_id", uid}, {"nick", sp->nick}});
      }
      // -------- AUTH (token) --------
      else if (op == "auth") {
        if (!g_pg.enabled) {
          sp->send({{"op", "error"}, {"reason", "pg_disabled"}});
          continue;
        }
        std::string token = in.value("token", "");
        if (token.empty()) {
          sp->send({{"op", "error"}, {"reason", "missing_token"}});
          continue;
        }
        auto uid = pg_validate_session(token);
        if (!uid) {
          sp->send({{"op", "error"}, {"reason", "token_invalid"}});
          continue;
        }
        auto uname =
            pg_username_by_id(*uid).value_or("user" + std::to_string(*uid));
        sp->authenticated = true;
        sp->user_id = *uid;
        sp->nick = uname;
        sp->send({{"op", "auth_ok"}, {"user_id", *uid}, {"nick", sp->nick}});
      }
      // -------- GUILDS --------
      else if (op == "guilds_list") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        pg_guilds_ensure_default_for_user(sp->user_id, sp->nick);
        auto list = pg_guilds_for_user(sp->user_id);
        sp->send({{"op", "guilds_list_ok"}, {"guilds", list}});
      } else if (op == "guild_create") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        std::string name = in.value("name", "");
        if (name.empty()) {
          sp->send({{"op", "error"}, {"reason", "missing_name"}});
          continue;
        }
        long gid = pg_guild_create(name, sp->user_id, sp->nick);
        sp->send({{"op", "guild_created"}, {"id", gid}, {"name", name}});
      }
      // -------- CHANNELS (per guild) --------
      else if (op == "channels_list_guild") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        long gid = in.value("guild_id", 0L);
        if (!gid) {
          sp->send({{"op", "error"}, {"reason", "bad_guild"}});
          continue;
        }
        auto sets = pg_channels_for_guild(gid);
        json payload = {{"op", "channels_list_ok"}, {"guild_id", gid}};
        payload.update(sets);
        sp->send(payload);
      } else if (op == "channel_create") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        long gid = in.value("guild_id", 0L);
        std::string kind =
            in.value("kind", "text"); // 'text'|'category'|'voice'
        std::string name = in.value("name", "");
        int position = in.value("position", 0);
        std::optional<long> parent;
        if (in.contains("parent_id"))
          parent = in["parent_id"].get<long>();
        if (!gid || name.empty()) {
          sp->send({{"op", "error"}, {"reason", "missing_fields"}});
          continue;
        }
        long cid = pg_channel_create(gid, parent, kind, name, position, false);
        sp->send({{"op", "channel_created"},
                  {"id", cid},
                  {"guild_id", gid},
                  {"parent_id", parent ? *parent : 0},
                  {"kind", kind},
                  {"name", name},
                  {"position", position}});
      }
      // ---- CHANNEL: rename
      // -------------------------------------------------------
      else if (op == "channel_rename") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        long cid = in.value("channel_id", 0L);
        std::string name = in.value("name", "");
        if (!cid || name.empty()) {
          sp->send({{"op", "error"}, {"reason", "missing_fields"}});
          continue;
        }

        // (Optional) check: user must be guild member — we skip advanced perms
        // for now

        auto res = pg_channel_rename(cid, name);
        if (!res) {
          sp->send({{"op", "error"}, {"reason", "rename_failed"}});
          continue;
        }
        sp->send({{"op", "channel_updated"},
                  {"id", res->first},
                  {"name", res->second}});
      }
      // ---- CHANNEL: delete
      // -------------------------------------------------------
      else if (op == "channel_delete") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        long cid = in.value("channel_id", 0L);
        if (!cid) {
          sp->send({{"op", "error"}, {"reason", "bad_channel"}});
          continue;
        }

        // (Optional) check: user must be guild admin — skipped for now

        if (!pg_channel_delete(cid)) {
          sp->send({{"op", "error"}, {"reason", "delete_failed"}});
          continue;
        }
        sp->send({{"op", "channel_deleted"}, {"id", cid}});
      }
      // ---- CHANNEL: move/reorder
      // -------------------------------------------------
      else if (op == "channel_move") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        long cid = in.value("channel_id", 0L);
        int position = in.value("position", 0);
        std::optional<long> parent;
        if (in.contains("parent_id"))
          parent = in["parent_id"].get<long>();
        if (!cid) {
          sp->send({{"op", "error"}, {"reason", "bad_channel"}});
          continue;
        }

        auto res = pg_channel_move(cid, parent, position);
        if (!res) {
          sp->send({{"op", "error"}, {"reason", "move_failed"}});
          continue;
        }
        auto [id, pid, pos] = *res;
        sp->send({{"op", "channel_moved"},
                  {"id", id},
                  {"parent_id", pid},
                  {"position", pos}});
      }
      // -------- JOIN / LEAVE --------
      else if (op == "join") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        long cid = in.value("channel_id", 0L);
        if (!cid) {
          std::string name = in.value("channel", "general");
          cid = pg_channel_id_by_name(name);
        }
        if (!cid || !pg_channel_is_joinable(cid, sp->user_id)) {
          sp->send({{"op", "error"}, {"reason", "not_allowed"}});
          continue;
        }
        auto key = chkey(cid);
        sp->join_key(key);
        redis_room_add(cid, sp->user_id);
        g_channels.broadcast(key, {{"op", "joined"},
                                   {"channel_id", cid},
                                   {"user", sp->nick},
                                   {"user_id", sp->user_id}});
        // roster to joiner
        json users = json::array();
        for (auto uid : redis_room_members(cid))
          if (auto u = pg_username_by_id(uid))
            users.push_back({{"user_id", uid}, {"nick", *u}});
        std::string cname = pg_channel_name_by_id(cid).value_or("general");
        sp->send({{"op", "joined_ok"},
                  {"channel_id", cid},
                  {"name", cname},
                  {"you", {{"user_id", sp->user_id}, {"nick", sp->nick}}},
                  {"users", users}});
        broadcast_roster_snapshot(cid);
        // proactive latest history
        auto rows = pg_hist_latest(cid, HISTORY_PAGE_DEFAULT);
        json items = json::array();
        for (auto &r : rows) {
          json it;
          it["message_id"] = r.message_id;
          it["username"] = r.user;
          it["body"] = r.text;
          it["ts"] = r.ts;
          items.push_back(it);
        }
        sp->send({{"op", "history_ok"},
                  {"mode", "latest"},
                  {"channel_id", cid},
                  {"items", items}});
      } else if (op == "leave") {
        long cid = in.value("channel_id", 0L);
        auto key = chkey(cid);
        sp->leave_key(key);
        redis_room_del(cid, sp->user_id);
        g_channels.broadcast(key, {{"op", "left"},
                                   {"channel_id", cid},
                                   {"user", sp->nick},
                                   {"user_id", sp->user_id}});
        broadcast_roster_snapshot(cid);
        sp->send({{"op", "left_ok"}, {"channel_id", cid}});
      } else if (op == "presence_get") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        long cid = in.value("channel_id", 0L);
        json users = json::array();
        for (auto uid : redis_room_members(cid))
          if (auto u = pg_username_by_id(uid))
            users.push_back({{"user_id", uid}, {"nick", *u}});
        sp->send({{"op", "presence"}, {"channel_id", cid}, {"users", users}});
      }
      // -------- MSG --------
      else if (op == "msg") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        long cid = in.value("channel_id", 0L);
        std::string text = in.value("text", in.value("body", ""));
        if (cid <= 0 || text.empty())
          continue;
        // derive guild_id by lookup from channels
        long gid = 0;
        try {
          pqxx::work tx{*g_pg.conn};
          auto r =
              tx.exec_params("SELECT guild_id FROM channels WHERE id=$1", cid);
          gid = r.empty() ? 0 : r[0]["guild_id"].as<long>();
        } catch (...) {
        }
        long long ts = (long long)std::time(nullptr);
        long long id = 0;
        try {
          id = pg_insert_message_id(gid, cid, sp->nick, text, ts);
        } catch (const std::exception &ex) {
          std::cerr << "[pg] insert failed (non-fatal): " << ex.what() << "\n";
          sp->send({{"op", "error"}, {"reason", "db_insert_failed"}});
          continue;
        }
        auto key = chkey(cid);
        g_channels.broadcast(key, {{"op", "message"},
                                   {"channel_id", cid},
                                   {"message_id", id},
                                   {"id", id},
                                   {"user_id", sp->user_id},
                                   {"user", sp->nick},
                                   {"text", text},
                                   {"ts", ts}});
      }
      // -------- HISTORY (latest) --------
      else if (op == "history") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        long cid = in.value("channel_id", 0L);
        int limit = in.value("limit", 200);
        auto rows = pg_hist_latest(cid, limit);
        json items = json::array();
        for (auto &r : rows) {
          json it;
          it["message_id"] = r.message_id;
          it["username"] = r.user;
          it["body"] = r.text;
          it["ts"] = r.ts;
          items.push_back(it);
        }
        sp->send({{"op", "history_ok"},
                  {"mode", "latest"},
                  {"channel_id", cid},
                  {"items", items}});
      }
      // -------- HISTORY (before) --------
      else if (op == "history_before") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        long cid = in.value("channel_id", 0L);
        long long before_id = in.value("before_id", 0LL);
        int limit = in.value("limit", 200);
        auto rows = pg_hist_before(cid, before_id, limit);
        json items = json::array();
        for (auto &r : rows) {
          json it;
          it["message_id"] = r.message_id;
          it["username"] = r.user;
          it["body"] = r.text;
          it["ts"] = r.ts;
          items.push_back(it);
        }
        sp->send({{"op", "history_ok"},
                  {"mode", "before"},
                  {"channel_id", cid},
                  {"items", items}});
      }
      // -------- HISTORY (since) --------
      else if (op == "history_since") {
        if (!sp->authenticated) {
          sp->send({{"op", "error"}, {"reason", "auth_required"}});
          continue;
        }
        long cid = in.value("channel_id", 0L);
        long long after_id = in.value("after_id", 0LL);
        int limit = in.value("limit", 500);
        auto rows = pg_hist_since(cid, after_id, limit);
        json items = json::array();
        for (auto &r : rows) {
          json it;
          it["message_id"] = r.message_id;
          it["username"] = r.user;
          it["body"] = r.text;
          it["ts"] = r.ts;
          items.push_back(it);
        }
        sp->send({{"op", "history_ok"},
                  {"mode", "since"},
                  {"channel_id", cid},
                  {"items", items}});
      }
      // -------- PING --------
      else if (op == "ping") {
        if (sp->authenticated)
          redis_set_online(sp->user_id);
        sp->send({{"op", "pong"}});
      }
      // -------- UNKNOWN --------
      else {
        std::cerr << "[op] unknown: " << op << "\n";
        sp->send({{"op","error"},{"reason","unknown_op"}});
      }
    }
  } catch (const beast::system_error &se) {
    std::cerr << "[" << now_stamp() << "] disconnect " << who
              << " sid=" << sp->sid << " reason=" << se.what() << "\n";
  } catch (const std::exception &ex) {
    std::cerr << "[" << now_stamp() << "] disconnect " << who
              << " sid=" << sp->sid << " fatal=" << ex.what() << "\n";
  }
  sp->leave_all();
}

// ===== MAIN
// ==========================================================================
int main() {
  try {
    const char *env_bind = std::getenv("BIND_ADDR");
    const char *env_port = std::getenv("PORT");
    std::string bind_addr = env_bind ? env_bind : "0.0.0.0";
    unsigned short port =
        static_cast<unsigned short>(env_port ? std::atoi(env_port) : 9000);

    // Redis
    if (const char *r = std::getenv("REDIS_URL"); r && *r) {
      try {
        g_redis.conn = std::make_unique<sw::redis::Redis>(std::string(r));
        g_redis.enabled = true;
        std::cerr << "[" << now_stamp() << "] Redis connected\n";
      } catch (const std::exception &ex) {
        std::cerr << "[" << now_stamp() << "] Redis disabled: " << ex.what()
                  << "\n";
        g_redis.enabled = false;
      }
    } else
      std::cerr << "[" << now_stamp()
                << "] Redis disabled (REDIS_URL not set)\n";

    // Postgres
    try {
      const char *h = getenv("PGHOST"), *db = getenv("PGDATABASE"),
                 *u = getenv("PGUSER"), *p = getenv("PGPASSWORD"),
                 *po = getenv("PGPORT");
      if (h && db && u && p) {
        std::string conn = "host=" + std::string(h) + " dbname=" + db +
                           " user=" + u + " password=" + p +
                           (po ? (" port=" + std::string(po)) : "");
        g_pg.conn = std::make_unique<pqxx::connection>(conn);
        g_pg.enabled = g_pg.conn->is_open();
        if (g_pg.enabled) {
          std::cerr << "[" << now_stamp() << "] Postgres connected\n";
          pg_init_auth_schema();
          pg_init_channel_schema();
          pg_init_msg_schema();
        } else
          std::cerr << "[" << now_stamp() << "] Postgres disabled (not open)\n";
      } else
        std::cerr << "[" << now_stamp()
                  << "] Postgres disabled (env not set)\n";
    } catch (const std::exception &ex) {
      std::cerr << "[" << now_stamp() << "] Postgres disabled: " << ex.what()
                << "\n";
      g_pg.enabled = false;
    }

    net::io_context ioc;
    net::signal_set sigs(ioc, SIGINT, SIGTERM);
    sigs.async_wait([&](auto, auto) { ioc.stop(); });

    tcp::endpoint ep(net::ip::make_address(bind_addr), port);
    tcp::acceptor acceptor(ioc);
    acceptor.open(ep.protocol());
    acceptor.set_option(net::socket_base::reuse_address(true));
    acceptor.bind(ep);
    acceptor.listen();
    std::cerr << "[" << now_stamp() << "] BCord WS listening on " << ep << "\n";

    while (!ioc.stopped()) {
      tcp::socket s(ioc);
      boost::system::error_code ec;
      acceptor.accept(s, ec);
      if (ec) {
        std::cerr << "[" << now_stamp() << "] accept error: " << ec.message()
                  << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      std::thread(session_loop, std::move(s)).detach();
    }
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "[" << now_stamp() << "] Fatal: " << ex.what() << "\n";
    return 1;
  }
}

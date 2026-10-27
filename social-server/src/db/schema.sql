-- Applied on every startup. Statements must stay idempotent.

CREATE TABLE IF NOT EXISTS users (
  id         INTEGER PRIMARY KEY,
  name       TEXT    NOT NULL UNIQUE COLLATE NOCASE,
  role       TEXT    NOT NULL DEFAULT 'user' CHECK (role IN ('user', 'admin')),
  banned     INTEGER NOT NULL DEFAULT 0 CHECK (banned IN (0, 1)),
  created_at INTEGER NOT NULL,
  -- Owners may post News and Change Log from the client. Databases created
  -- before this column get it from openDb().
  rank       TEXT    NOT NULL DEFAULT 'user' CHECK (rank IN ('user', 'owner'))
);

-- Users and keys are separate so other login methods can be added later.
CREATE TABLE IF NOT EXISTS licenses (
  id           INTEGER PRIMARY KEY,
  key_hash     TEXT    NOT NULL UNIQUE,
  user_id      INTEGER NOT NULL REFERENCES users (id),
  status       TEXT    NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'revoked')),
  created_at   INTEGER NOT NULL,
  last_used_at INTEGER
);
CREATE INDEX IF NOT EXISTS licenses_user ON licenses (user_id);

CREATE TABLE IF NOT EXISTS sessions (
  token_hash TEXT    PRIMARY KEY,
  user_id    INTEGER NOT NULL REFERENCES users (id),
  expires_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS sessions_user ON sessions (user_id);
CREATE INDEX IF NOT EXISTS sessions_expires ON sessions (expires_at);

-- One row per pair, stored with user_a < user_b.
CREATE TABLE IF NOT EXISTS friendships (
  user_a     INTEGER NOT NULL REFERENCES users (id),
  user_b     INTEGER NOT NULL REFERENCES users (id),
  requester  INTEGER NOT NULL REFERENCES users (id),
  status     TEXT    NOT NULL CHECK (status IN ('pending', 'accepted')),
  created_at INTEGER NOT NULL,
  PRIMARY KEY (user_a, user_b),
  CHECK (user_a < user_b),
  CHECK (requester IN (user_a, user_b))
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS friendships_user_b ON friendships (user_b);

-- Messages are only soft-deleted, so ids keep increasing and work as history cursors.
CREATE TABLE IF NOT EXISTS global_messages (
  id         INTEGER PRIMARY KEY,
  author_id  INTEGER NOT NULL REFERENCES users (id),
  body       TEXT    NOT NULL,
  created_at INTEGER NOT NULL,
  deleted    INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0, 1))
);

CREATE TABLE IF NOT EXISTS dm_messages (
  id           INTEGER PRIMARY KEY,
  sender_id    INTEGER NOT NULL REFERENCES users (id),
  recipient_id INTEGER NOT NULL REFERENCES users (id),
  body         TEXT    NOT NULL,
  created_at   INTEGER NOT NULL,
  deleted      INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0, 1))
);
-- Serves both directions of a conversation and the per-sender unread count.
CREATE INDEX IF NOT EXISTS dm_messages_pair ON dm_messages (sender_id, recipient_id, id);

CREATE TABLE IF NOT EXISTS dm_reads (
  reader_id INTEGER NOT NULL REFERENCES users (id),
  other_id  INTEGER NOT NULL REFERENCES users (id),
  up_to     INTEGER NOT NULL,
  PRIMARY KEY (reader_id, other_id)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS news (
  id         INTEGER PRIMARY KEY,
  kind       TEXT    NOT NULL CHECK (kind IN ('news', 'changelog')),
  title      TEXT    NOT NULL,
  body       TEXT    NOT NULL,
  version    TEXT,
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS news_kind ON news (kind, id);

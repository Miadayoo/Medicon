import type { Db } from '../db/db.ts'
import type { ErrorCode } from '../errors.ts'
import { readUpTo, unreadCount } from './messages.ts'
import type { UserRef } from './users.ts'

// Spec 2.3: how another user relates to me.
export type Relation = 'none' | 'friend' | 'outgoing' | 'incoming'

export interface Friend extends UserRef {
  online: boolean
  unread: number
  theirReadUpTo: number
}

export interface FriendList {
  friends: Friend[]
  incoming: UserRef[]
  outgoing: UserRef[]
}

interface PairRow {
  requester: number
  status: 'pending' | 'accepted'
}

// Pairs are stored once, with user_a < user_b (spec 3.3).
const ordered = (a: number, b: number): [number, number] => (a < b ? [a, b] : [b, a])

function pair(db: Db, me: number, other: number): PairRow | undefined {
  const [a, b] = ordered(me, other)
  return db
    .prepare<[number, number], PairRow>('SELECT requester, status FROM friendships WHERE user_a = ? AND user_b = ?')
    .get(a, b)
}

export function relation(db: Db, me: number, other: number): Relation {
  const row = pair(db, me, other)
  if (!row) return 'none'
  if (row.status === 'accepted') return 'friend'
  return row.requester === me ? 'outgoing' : 'incoming'
}

export function areFriends(db: Db, me: number, other: number): boolean {
  return relation(db, me, other) === 'friend'
}

function findUser(db: Db, id: number): UserRef | undefined {
  return db.prepare<[number], UserRef>('SELECT id, name FROM users WHERE id = ?').get(id)
}

function findUserNamed(db: Db, name: string): UserRef | undefined {
  return db.prepare<[string], UserRef>('SELECT id, name FROM users WHERE name = ?').get(name)
}

function remove(db: Db, me: number, other: number): void {
  const [a, b] = ordered(me, other)
  db.prepare('DELETE FROM friendships WHERE user_a = ? AND user_b = ?').run(a, b)
}

function accept(db: Db, me: number, other: number): void {
  const [a, b] = ordered(me, other)
  db.prepare("UPDATE friendships SET status = 'accepted' WHERE user_a = ? AND user_b = ?").run(a, b)
}

type Failure = { ok: false; error: ErrorCode }

// Spec 3.5 friend.request. A request the other user already sent me is accepted at once.
export type RequestResult = { ok: true; other: UserRef; accepted: boolean } | Failure

export function requestFriend(db: Db, me: number, name: string, now: number): RequestResult {
  return db.transaction((): RequestResult => {
    const other = findUserNamed(db, name)
    if (!other) return { ok: false, error: 'user_not_found' }
    if (other.id === me) return { ok: false, error: 'self' }
    switch (relation(db, me, other.id)) {
      case 'friend':
        return { ok: false, error: 'already_friends' }
      case 'outgoing':
        return { ok: false, error: 'request_exists' }
      case 'incoming':
        accept(db, me, other.id)
        return { ok: true, other, accepted: true }
      case 'none': {
        const [a, b] = ordered(me, other.id)
        db.prepare("INSERT INTO friendships (user_a, user_b, requester, status, created_at) VALUES (?, ?, ?, 'pending', ?)").run(
          a,
          b,
          me,
          now,
        )
        return { ok: true, other, accepted: false }
      }
    }
  })()
}

// Spec 3.5 friend.respond: only a request the other user sent me.
export type RespondResult = { ok: true; other: UserRef } | Failure

export function respondFriend(db: Db, me: number, otherId: number, accepted: boolean): RespondResult {
  return db.transaction((): RespondResult => {
    const other = findUser(db, otherId)
    if (!other) return { ok: false, error: 'user_not_found' }
    if (relation(db, me, otherId) !== 'incoming') return { ok: false, error: 'bad_request' }
    if (accepted) accept(db, me, otherId)
    else remove(db, me, otherId)
    return { ok: true, other }
  })()
}

// Spec 3.5 friend.remove: a friend (removed) or my own pending request (cancelled).
export type RemoveResult = { ok: true; other: UserRef; status: 'removed' | 'cancelled' } | Failure

export function removeFriend(db: Db, me: number, otherId: number): RemoveResult {
  return db.transaction((): RemoveResult => {
    const other = findUser(db, otherId)
    if (!other) return { ok: false, error: 'user_not_found' }
    const current = relation(db, me, otherId)
    if (current !== 'friend' && current !== 'outgoing') return { ok: false, error: 'not_friends' }
    remove(db, me, otherId)
    return { ok: true, other, status: current === 'friend' ? 'removed' : 'cancelled' }
  })()
}

// Accepted friends' ids, for presence.
export function friendIds(db: Db, me: number): number[] {
  return db
    .prepare<[number, number, number], { id: number }>(
      `SELECT CASE WHEN user_a = ? THEN user_b ELSE user_a END AS id
       FROM friendships WHERE status = 'accepted' AND (user_a = ? OR user_b = ?)`,
    )
    .all(me, me, me)
    .map((row) => row.id)
}

interface ListRow {
  id: number
  name: string
  requester: number
  status: 'pending' | 'accepted'
}

export function listFriends(db: Db, me: number, isOnline: (userId: number) => boolean): FriendList {
  const rows = db
    .prepare<[number, number, number], ListRow>(
      `SELECT u.id, u.name, f.requester, f.status
       FROM friendships f JOIN users u ON u.id = CASE WHEN f.user_a = ? THEN f.user_b ELSE f.user_a END
       WHERE f.user_a = ? OR f.user_b = ?
       ORDER BY u.name COLLATE NOCASE`,
    )
    .all(me, me, me)
  const list: FriendList = { friends: [], incoming: [], outgoing: [] }
  for (const row of rows) {
    const user = { id: row.id, name: row.name }
    if (row.status === 'accepted') {
      list.friends.push({
        ...user,
        online: isOnline(row.id),
        unread: unreadCount(db, me, row.id),
        theirReadUpTo: readUpTo(db, row.id, me),
      })
    } else if (row.requester === me) {
      list.outgoing.push(user)
    } else {
      list.incoming.push(user)
    }
  }
  return list
}

export interface SearchItem extends UserRef {
  relation: Relation
}

// Spec 2.3: username prefix, at most 10, never me. LIKE wildcards in the query are literal.
export function searchUsers(db: Db, me: number, prefix: string): SearchItem[] {
  const pattern = `${prefix.replace(/[\\%_]/g, (c) => `\\${c}`)}%`
  return db
    .prepare<[number, string], UserRef>(
      `SELECT id, name FROM users WHERE id != ? AND banned = 0 AND name LIKE ? ESCAPE '\\' COLLATE NOCASE
       ORDER BY name COLLATE NOCASE LIMIT 10`,
    )
    .all(me, pattern)
    .map((user) => ({ ...user, relation: relation(db, me, user.id) }))
}

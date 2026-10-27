import { z } from 'zod'
import type { ErrorCode } from '../errors.ts'
import type { DmMessage, Message } from '../services/messages.ts'
import type { NewsItem } from '../services/news.ts'
import type { Member, UserRef } from '../services/users.ts'

// Spec 2.6.
export const CLOSE = {
  goingAway: 1001,
  tokenInvalid: 4001,
  kicked: 4003,
  replaced: 4009,
} as const

// The nonce a client attaches to requests that expect an ack.
export const requestId = z.string().min(1).max(64)

const userId = z.number().int().positive()
// Bodies are measured after trimming (checkBody); this only bounds the frame.
const body = z.string().max(4000)

// Client -> server frames (spec 2.4). Anything else is answered with ack bad_request.
export const clientMessage = z.discriminatedUnion('t', [
  z.object({ t: z.literal('ping') }),
  z.object({ t: z.literal('global.send'), id: requestId, body }),
  z.object({ t: z.literal('dm.send'), id: requestId, to: userId, body }),
  z.object({ t: z.literal('dm.read'), with: userId, upTo: z.number().int().nonnegative() }),
  z.object({ t: z.literal('friend.request'), id: requestId, name: z.string().min(1).max(32) }),
  z.object({ t: z.literal('friend.respond'), id: requestId, userId, accept: z.boolean() }),
  z.object({ t: z.literal('friend.remove'), id: requestId, userId }),
  // Owners only (rank owner): the same News / Change Log as the admin CLI's news:post.
  z.object({ t: z.literal('news.post'), id: requestId, kind: z.enum(['news', 'changelog']), title: z.string(), body: z.string(), version: z.string().optional() }),
])
export type ClientMessage = z.infer<typeof clientMessage>
// The frames handled outside the hub itself.
export type RequestMessage = Exclude<ClientMessage, { t: 'ping' }>

export type KickReason = 'revoked' | 'banned'

export type FriendStatus = 'accepted' | 'declined' | 'removed' | 'cancelled'

export type ServerMessage =
  | { t: 'hello'; user: Member; serverTime: number }
  | { t: 'ack'; id: string; ok: true; message?: Message | DmMessage }
  | { t: 'ack'; id: string; ok: false; error: ErrorCode }
  | { t: 'pong' }
  | { t: 'global.new'; message: Message }
  | { t: 'dm.new'; message: DmMessage }
  | { t: 'dm.read'; by: number; upTo: number }
  | { t: 'friend.request'; from: UserRef }
  | { t: 'friend.update'; user: UserRef; status: FriendStatus }
  | { t: 'presence'; userId: number; online: boolean }
  | { t: 'news.new'; item: NewsItem }
  | { t: 'kicked'; reason: KickReason }

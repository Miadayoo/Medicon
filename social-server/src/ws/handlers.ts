import type { AppContext } from '../context.ts'
import { areFriends, friendIds, removeFriend, requestFriend, respondFriend } from '../services/friends.ts'
import { checkBody, createDm, createGlobalMessage, markRead } from '../services/messages.ts'
import { createNews, newsInput } from '../services/news.ts'
import type { Member, UserRef } from '../services/users.ts'
import type { HubEvents } from './hub.ts'
import type { RequestMessage, ServerMessage } from './protocol.ts'

// Spec 3.5: Global, DM and friend requests, and presence for friends.
export function createHandlers(ctx: AppContext): HubEvents {
  const { db, hub } = ctx

  // Each newly connected friend sees the other online, both ways.
  const announceFriendship = (a: UserRef, b: UserRef): void => {
    if (hub.isOnline(b.id)) hub.send(a.id, { t: 'presence', userId: b.id, online: true })
    if (hub.isOnline(a.id)) hub.send(b.id, { t: 'presence', userId: a.id, online: true })
  }

  const request = (member: Member, message: RequestMessage, reply: (frame: ServerMessage) => void): void => {
    // Friend frames name users without their rank; messages carry it.
    const user: UserRef = { id: member.id, name: member.name }
    const fail = (id: string, error: Extract<ServerMessage, { ok: false }>['error']): void =>
      reply({ t: 'ack', id, ok: false, error })
    switch (message.t) {
      case 'global.send': {
        const body = checkBody(message.body)
        if (!body.ok) return fail(message.id, body.error)
        if (!ctx.limits.global.tryHit(String(user.id), ctx.now())) return fail(message.id, 'rate_limited')
        const saved = createGlobalMessage(db, member, body.body, ctx.now())
        reply({ t: 'ack', id: message.id, ok: true, message: saved })
        hub.broadcast({ t: 'global.new', message: saved })
        return
      }
      case 'dm.send': {
        const body = checkBody(message.body)
        if (!body.ok) return fail(message.id, body.error)
        if (!areFriends(db, user.id, message.to)) return fail(message.id, 'not_friends')
        if (!ctx.limits.dm.tryHit(String(user.id), ctx.now())) return fail(message.id, 'rate_limited')
        const saved = createDm(db, member, message.to, body.body, ctx.now())
        reply({ t: 'ack', id: message.id, ok: true, message: saved })
        hub.send(message.to, { t: 'dm.new', message: saved })
        return
      }
      case 'dm.read': {
        // No ack (spec 2.4). Reading a non-friend's conversation does nothing.
        if (!areFriends(db, user.id, message.with)) return
        const upTo = markRead(db, user.id, message.with, message.upTo)
        hub.send(message.with, { t: 'dm.read', by: user.id, upTo })
        return
      }
      case 'news.post': {
        if (member.rank !== 'owner') return fail(message.id, 'forbidden')
        const input = newsInput.safeParse(message)
        if (!input.success) return fail(message.id, 'bad_request')
        const item = createNews(db, input.data, ctx.now())
        reply({ t: 'ack', id: message.id, ok: true })
        hub.broadcast({ t: 'news.new', item })
        return
      }
      case 'friend.request': {
        if (!ctx.limits.friend.tryHit(String(user.id), ctx.now())) return fail(message.id, 'rate_limited')
        const result = requestFriend(db, user.id, message.name, ctx.now())
        if (!result.ok) return fail(message.id, result.error)
        reply({ t: 'ack', id: message.id, ok: true })
        if (result.accepted) {
          // They had already asked me: we are friends now (spec 3.5).
          hub.send(user.id, { t: 'friend.update', user: result.other, status: 'accepted' })
          hub.send(result.other.id, { t: 'friend.update', user, status: 'accepted' })
          announceFriendship(user, result.other)
        } else {
          hub.send(result.other.id, { t: 'friend.request', from: user })
        }
        return
      }
      case 'friend.respond': {
        const result = respondFriend(db, user.id, message.userId, message.accept)
        if (!result.ok) return fail(message.id, result.error)
        reply({ t: 'ack', id: message.id, ok: true })
        if (message.accept) {
          hub.send(user.id, { t: 'friend.update', user: result.other, status: 'accepted' })
          hub.send(result.other.id, { t: 'friend.update', user, status: 'accepted' })
          announceFriendship(user, result.other)
        } else {
          hub.send(result.other.id, { t: 'friend.update', user, status: 'declined' })
        }
        return
      }
      case 'friend.remove': {
        const result = removeFriend(db, user.id, message.userId)
        if (!result.ok) return fail(message.id, result.error)
        reply({ t: 'ack', id: message.id, ok: true })
        if (result.status === 'removed') hub.send(user.id, { t: 'friend.update', user: result.other, status: 'removed' })
        hub.send(result.other.id, { t: 'friend.update', user, status: result.status })
        return
      }
    }
  }

  // Spec 3.5: on connect and disconnect, tell the friends who are online.
  const presence = (user: UserRef, online: boolean): void => {
    for (const id of friendIds(db, user.id)) hub.send(id, { t: 'presence', userId: user.id, online })
  }

  return { request, presence }
}

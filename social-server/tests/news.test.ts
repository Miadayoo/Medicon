import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { createNews } from '../src/services/news.ts'
import { http, startServer, type TestServer } from './helpers.ts'

let server: TestServer
let token: string

beforeEach(async () => {
  server = await startServer()
  token = await server.signUp('faokun')
})

afterEach(async () => {
  await server.close()
})

const getNews = (query: string) =>
  http(`${server.baseUrl}/news${query}`, { headers: { Authorization: `Bearer ${token}` } })

describe('GET /news', () => {
  it('lists one kind, newest first', async () => {
    const { db } = server.ctx
    createNews(db, { kind: 'news', title: 'first', body: 'a' }, 1000)
    createNews(db, { kind: 'changelog', title: 'v1', body: 'b', version: '1.0.0' }, 2000)
    createNews(db, { kind: 'news', title: 'second', body: 'c\nd' }, 3000)

    const news = await getNews('')
    expect(news.status).toBe(200)
    expect(news.body.items).toEqual([
      { id: 3, kind: 'news', title: 'second', body: 'c\nd', version: null, createdAt: 3000 },
      { id: 1, kind: 'news', title: 'first', body: 'a', version: null, createdAt: 1000 },
    ])
    const changelog = await getNews('?kind=changelog')
    expect(changelog.body.items).toEqual([
      { id: 2, kind: 'changelog', title: 'v1', body: 'b', version: '1.0.0', createdAt: 2000 },
    ])
  })

  it('pages with limit and before', async () => {
    for (let i = 1; i <= 5; i++) createNews(server.ctx.db, { kind: 'news', title: `n${i}`, body: 'x' }, i)
    const ids = async (query: string) => ((await getNews(query)).body.items as { id: number }[]).map((item) => item.id)

    expect(await ids('?limit=2')).toEqual([5, 4])
    expect(await ids('?limit=2&before=4')).toEqual([3, 2])
    expect(await ids('?limit=2&before=2')).toEqual([1])
    expect(await ids('?before=1')).toEqual([])
  })

  it('defaults to 20 items and clamps limit to 100', async () => {
    for (let i = 1; i <= 120; i++) createNews(server.ctx.db, { kind: 'news', title: `n${i}`, body: 'x' }, i)
    expect((await getNews('')).body.items).toHaveLength(20)
    expect((await getNews('?limit=500')).body.items).toHaveLength(100)
  })

  it('rejects invalid query values with 400 bad_request', async () => {
    for (const query of ['?kind=other', '?limit=0', '?limit=abc', '?limit=1.5', '?before=-1', '?before=x']) {
      expect(await getNews(query)).toEqual({ status: 400, body: { error: 'bad_request' } })
    }
  })
})

describe('admin POST /news', () => {
  it('stores the item and normalizes line endings', async () => {
    const result = await server.admin('/news', { kind: 'changelog', title: ' v2 ', body: 'line1\r\nline2\r\n', version: '2.0.0' })
    expect(result.status).toBe(200)
    expect(result.body.item).toMatchObject({ id: 1, kind: 'changelog', title: 'v2', body: 'line1\nline2', version: '2.0.0' })
    expect((await getNews('?kind=changelog')).body.items[0]).toEqual(result.body.item)
  })

  it('rejects invalid items', async () => {
    const bad = [
      { kind: 'other', title: 't', body: 'b' },
      { kind: 'news', title: '   ', body: 'b' },
      { kind: 'news', title: 't', body: ' \r\n ' },
      { kind: 'news', title: 't' },
    ]
    for (const body of bad) expect(await server.admin('/news', body)).toEqual({ status: 400, body: { error: 'bad_request' } })
  })
})

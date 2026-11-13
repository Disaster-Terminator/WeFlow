const assert = require('node:assert/strict')
const { spawnSync } = require('node:child_process')
const { createCipheriv, createHash } = require('node:crypto')
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const { Worker } = require('node:worker_threads')
const { zstdCompressSync } = require('node:zlib')
const { buildSync } = require('esbuild')

const root = path.resolve(__dirname, '..')
const resources = path.resolve(process.argv[2] || path.join(root, 'resources'))
const native = path.join(resources, 'native', 'win32-x64')
const shell = path.join(root, 'out', 'native', 'sqlite3mc.exe')
const binaryPassphrase = process.env.WEFLOW_TEST_BINARY_PASSPHRASE === '1'
const passphrase = 'synthetic-database-passphrase-01'
const key = binaryPassphrase ? Buffer.from(passphrase).toString('hex') : 'd1'.repeat(32)
const aesKey = '0123456789abcdef'
const xorKey = 0xb4
const fixture = fs.mkdtempSync(path.join(os.tmpdir(), 'weflow-native-'))
const account = path.join(fixture, '测试账号', 'wxid_synthetic_me')
const sessionDb = path.join(account, 'db_storage', 'session', 'session.db')
const messages = path.join(account, 'db_storage', 'message')
const snsDb = path.join(account, 'db_storage', 'sns', 'sns.db')
const ids = ['wxid_synthetic_friend', 'synthetic@chatroom']
const year = 2025
const day = new Date(year, 4, 16, 12).getTime() / 1000
const imageMd5 = 'c'.repeat(32)
const videoMd5 = 'a'.repeat(32)
const media = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAACXBIWXMAAAPoAAAD6AG1e1JrAAAAEklEQVQImWNwbVro2rSQAUIBACX+BaHGJ5BXAAAAAElFTkSuQmCC', 'base64')
const mediaDir = path.join(account, 'msg', 'attach')
const dat = path.join(mediaDir, imageMd5 + '.dat')
const outputDir = path.join(fixture, 'output')
const groupContent = zstdCompressSync(Buffer.from('@Synthetic Group message')).toString('hex')

function sqlite(file, sql) {
  const result = spawnSync(shell, [path.basename(file)], {
    cwd: path.dirname(file),
    input: `PRAGMA cipher=sqlcipher; PRAGMA legacy=4; PRAGMA key="${binaryPassphrase ? passphrase : `x'${key}'`}";${sql}`,
    encoding: 'utf8'
  })
  assert.equal(result.status, 0, result.stderr || result.error?.message)
  assert.equal(fs.readFileSync(file).subarray(0, 16).equals(Buffer.from('SQLite format 3\0')), false)
}

async function runWorker(entry, workerData, selectResult, request) {
  const worker = new Worker(entry, { workerData })
  try {
    return await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`Worker timed out: ${entry}`)), 30_000)
      worker.on('message', (message) => {
        if (message.type === 'export:error' || message.type === 'annualReport:error') reject(new Error(message.error))
        if (selectResult(message)) resolve(message)
      })
      worker.once('error', reject)
      worker.once('exit', (code) => reject(new Error(`Worker exited with ${code}: ${entry}`)))
      worker.once('exit', () => clearTimeout(timer))
      for (const item of Array.isArray(request) ? request : request ? [request] : []) {
        worker.postMessage(item)
      }
    })
  } finally {
    await worker.terminate()
  }
}

async function checkBuiltWorkers() {
  const archive = path.join(resources, '..', 'app.asar')
  const installed = fs.existsSync(archive)
  const workerDir = installed ? path.join(archive, 'dist-electron') : path.join(root, 'dist-electron')
  if (!fs.existsSync(workerDir)) return
  const userDataPath = path.join(fixture, 'profile')
  const replies = new Map()
  await runWorker(path.join(workerDir, 'wcdbWorker.js'), {}, (message) => {
    replies.set(message.id, message)
    return message.id === 5
  }, [
    { id: 1, type: 'setPaths', payload: { resourcesPath: resources, userDataPath } },
    { id: 2, type: 'open', payload: { accountDir: account, hexKey: key } },
    { id: 3, type: 'getSessions' },
    { id: 4, type: 'getMessages', payload: { sessionId: ids[0], limit: 2, offset: 0 } },
    { id: 5, type: 'close' }
  ])
  assert.equal(replies.get(2).result, true)
  assert.deepEqual(replies.get(3).result.sessions.map((row) => row.username), ids)
  assert.equal(replies.get(4).result.messages[1].message_content, '你好')

  const decoded = await runWorker(path.join(workerDir, 'imageDecryptWorker.js'), {
    imageNativeAddonPath: path.join(native, 'image-decrypt.node')
  }, (message) => message.id === 1, { id: 1, datPath: dat, xorKey, aesKey })
  assert.equal(decoded.ok, true)
  assert.deepEqual(Buffer.from(decoded.data), media)
  const { data: pixels, info } = await require('sharp')(Buffer.from(decoded.data)).raw().toBuffer({ resolveWithObject: true })
  assert.deepEqual([info.width, info.height], [2, 2])
  assert.deepEqual([...pixels.subarray(0, 3)], [0x45, 0x82, 0xa1])

  const formattedDir = path.join(fixture, 'formatted')
  const { data } = await runWorker(path.join(workerDir, 'exportWorker.js'), {
    mode: 'sessions', sessionIds: ids, outputDir: formattedDir, resourcesPath: resources, userDataPath,
    dbPath: account, accountDir: account, myAccountId: 'wxid_synthetic_me', decryptKey: key,
    imageAesKey: aesKey, imageXorKey: xorKey, isPackaged: true,
    options: { format: 'json', exportImages: true, exportVideos: false, sessionLayout: 'per-session' }
  }, (message) => message.type === 'export:result')
  assert.equal(data.success, true, JSON.stringify(data))
  assert.equal(data.successCount, 2)
  for (const id of ids) {
    assert.equal(data.rawSessionStats[id].rows, 2)
    assert.equal(data.rawSessionStats[id].mediaFailures, 0)
  }
  const files = fs.readdirSync(formattedDir, { recursive: true })
  const texts = files.filter((file) => file.endsWith('.json')).map((file) => fs.readFileSync(path.join(formattedDir, file), 'utf8'))
  assert.ok(texts.some((text) => text.includes('你好')))
  assert.ok(texts.some((text) => text.includes('Group message')))
  const imageFile = files.find((file) => file.endsWith('.png'))
  assert.ok(imageFile)
  assert.deepEqual(fs.readFileSync(path.join(formattedDir, imageFile)), media)
  const annual = await runWorker(path.join(workerDir, 'annualReportWorker.js'), {
    year, dbPath: path.dirname(account), decryptKey: key, myAccountId: 'wxid_synthetic_me',
    resourcesPath: resources, userDataPath
  }, (message) => message.type === 'annualReport:result')
  assert.equal(annual.data.success, true, annual.data.error)
  assert.equal(annual.data.data.year, year)
  assert.equal(annual.data.data.totalMessages, 2)
  console.log(`${installed ? 'Installed' : 'Built'} Electron database, image, annual report and formatted export workers passed.`)
}

async function main() {
  fs.mkdirSync(path.dirname(sessionDb), { recursive: true })
  fs.mkdirSync(messages, { recursive: true })
  fs.mkdirSync(path.dirname(snsDb), { recursive: true })
  fs.mkdirSync(mediaDir, { recursive: true })
  sqlite(sessionDb, `CREATE TABLE SessionTable(username TEXT, sort_timestamp INTEGER);
    INSERT INTO SessionTable VALUES ('${ids[0]}', ${day + 30}), ('${ids[1]}', ${day + 20});`)

  const table = (id) => 'Msg_' + createHash('md5').update(id).digest('hex')
  const schema = (id) => `CREATE TABLE ${table(id)} (local_id INTEGER, server_id INTEGER,
    create_time INTEGER, sort_seq INTEGER, local_type INTEGER, real_sender_id INTEGER,
    message_content TEXT, image_dat_name TEXT, source TEXT);`
  const senders = "CREATE TABLE Name2Id(user_name TEXT); INSERT INTO Name2Id VALUES ('wxid_synthetic_me'), ('wxid_synthetic_friend');"
  sqlite(path.join(messages, 'message_0.db'), senders + schema(ids[0]) + schema(ids[1]) + `
    INSERT INTO ${table(ids[0])} VALUES (1, 123456789123456789, ${day + 10}, 10, 1, 2, '你好', NULL, NULL);
    INSERT INTO ${table(ids[1])} VALUES (1, 21, ${day + 20}, 20, 1, 2, x'${groupContent}', NULL,
      '<msgsource><atuserlist><![CDATA[wxid_synthetic_me]]></atuserlist></msgsource>'),
      (2, 22, ${day + 40}, 40, 43, 1, '<videomsg rawmd5="${videoMd5}"/>', NULL, NULL);`)
  sqlite(path.join(messages, 'message_1.db'), senders + schema(ids[0]) + `
    INSERT INTO ${table(ids[0])} VALUES (2, 31, ${day + 30}, 30, 3, 1, '<img md5="${imageMd5}"/>', '${imageMd5}.dat', NULL);`)
  sqlite(snsDb, `CREATE TABLE SnsTimeLine(tid INTEGER PRIMARY KEY, user_name TEXT, content TEXT);
    INSERT INTO SnsTimeLine VALUES (123456789123456789, '${ids[0]}',
      '<TimelineObject><createTime>${day}</createTime><contentDesc>Synthetic photo</contentDesc><ContentObject><contentStyle>1</contentStyle></ContentObject></TimelineObject>'),
    (33, 'wxid_synthetic_me',
      '<TimelineObject><createTime>${day + 100}</createTime><contentDesc>Synthetic text</contentDesc></TimelineObject>');`)

  const header = Buffer.from([7, 8, 86, 50, 8, 7, 13, 0, 0, 0, 9, 0, 0, 0, 0])
  const cipher = createCipheriv('aes-128-ecb', aesKey, null)
  const xor = (bytes) => Buffer.from(bytes.map((byte) => byte ^ xorKey))
  fs.writeFileSync(dat, Buffer.concat([header, cipher.update(media.subarray(0, 13)), cipher.final(),
    media.subarray(13, -9), xor(media.subarray(-9))]))

  const image = require(path.join(native, 'image-decrypt.node'))
  assert.deepEqual(image.decryptDatNative(dat, xorKey, aesKey).data, media)
  assert.throws(() => image.decryptDatNative(dat, xorKey, 'invalid key'))
  const legacyDat = path.join(fixture, 'legacy.dat')
  fs.writeFileSync(legacyDat, xor(media))
  assert.deepEqual(image.decryptDatNative(legacyDat, xorKey).data, media)

  const bundle = path.join(root, 'out', 'native', 'smoke')
  buildSync({
    entryPoints: ['wcdbCore', 'weliveBridge'].map((name) => path.join(root, 'electron/services', name + '.ts')),
    bundle: true, platform: 'node', format: 'cjs', packages: 'external', outdir: bundle
  })
  const { WcdbCore } = require(path.join(bundle, 'wcdbCore.js'))
  const core = new WcdbCore()
  core.setPaths(resources, fixture)
  try {
    const wrongKey = await core.open(account, '00'.repeat(32))
    assert.equal(wrongKey, false)
    assert.equal(await core.open(account, key), true)
    const second = new WcdbCore()
    second.setPaths(resources, fixture)
    try {
      assert.equal(await second.open(account, key), true)
    } finally {
      second.close()
    }
    assert.deepEqual((await core.getSessions()).sessions.map((row) => row.username), ids)
    const rows = (await core.getMessages(ids[0], 2, 0)).messages
    assert.deepEqual(rows.map((row) => row.local_id), [2, 1])
    assert.equal(rows[1].server_id, '123456789123456789')
    assert.equal(rows[1].message_content, '你好')
    assert.equal((await core.getMessageCount(ids[0])).count, 2)
    assert.deepEqual((await core.getAvailableYears(ids)).data, [year])
    const stats = await core.getAnnualReportStats(ids, day, day + 59)
    assert.deepEqual(stats.data, {
      total: 4,
      sessions: {
        [ids[0]]: { sent: 1, received: 1, monthly: { 5: 2 } },
        [ids[1]]: { sent: 1, received: 1, monthly: { 5: 2 } }
      },
      daily: { '2025-05-16': 4 }
    })
    assert.equal((await core.getAnnualReportStats(ids, day + 41, day + 59)).data.total, 0)
    const images = await core.getMediaStream({ sessionId: ids[0], mediaType: 'image', beginTimestamp: day })
    assert.equal(images.success, true, images.error)
    assert.equal(images.items.length, 1)
    assert.equal(images.items[0].imageMd5, imageMd5)
    assert.equal((await core.getMediaStream({ sessionId: ids[0], mediaType: 'video' })).items.length, 0)
    const allMedia = await core.getMediaStream({ mediaType: 'all' })
    assert.deepEqual(allMedia.items.map(item => item.sessionId), [ids[1], ids[0]])
    assert.equal(allMedia.items[0].videoMd5, videoMd5)
    assert.equal((await core.getMediaStream({ mediaType: 'all', offset: 1 })).items[0].imageMd5, imageMd5)
    assert.deepEqual((await core.getSnsUsernames()).usernames, [ids[0], 'wxid_synthetic_me'])
    assert.deepEqual((await core.getSnsExportStats('wxid_synthetic_me')).data,
      { totalPosts: 2, totalFriends: 2, myPosts: 1 })
    assert.equal((await core.getSnsTimeline(1, 1)).timeline[0].id, '123456789123456789')
    const moments = await core.getSnsTimeline(10, 0, [ids[0]], 'photo', day, day + 1)
    assert.equal(moments.success, true, moments.error)
    assert.equal(moments.timeline[0].contentDesc, 'Synthetic photo')
    const cursor = await core.openMessageCursor(ids[0], 1, true, 0, 0)
    assert.equal(cursor.success, true)
    assert.equal((await core.fetchMessageBatch(cursor.cursor)).rows[0].local_id, 1)
    assert.equal((await core.fetchMessageBatch(cursor.cursor)).rows[0].local_id, 2)
    await core.closeMessageCursor(cursor.cursor)
  } finally {
    core.close()
  }

  const { runWeliveExport } = require(path.join(bundle, 'weliveBridge.js'))
  const result = await runWeliveExport({
    resourcesPath: resources,
    request: {
      account: { sessionDb, dbKey: key, accountDir: account, myAccountId: 'wxid_synthetic_me', imageXorKey: xorKey, imageAesKey: aesKey },
      sessionIds: ids, outputDir, format: 'raw-jsonl', ascending: true, mediaTypes: ['image']
    }
  })
  assert.equal(result.success, true, result.error)
  assert.equal(result.successCount, 2)
  for (const [index, id] of ids.entries()) {
    const manifest = result.rawExportManifests[id]
    const data = fs.readFileSync(manifest.path)
    const rows = data.toString('utf8').trimEnd().split('\n').map(JSON.parse)
    assert.equal(manifest.bytes, data.length)
    assert.equal(manifest.rows, rows.length)
    assert.equal(manifest.images_failed, 0)
    assert.equal(rows[0].message_content, index === 0 ? '你好' : groupContent)
    if (index === 0) assert.deepEqual(fs.readFileSync(rows[1].media_path), media)
  }
  console.log('Encrypted session/message reads, media scan, Moments, annual statistics, DAT decoding and export manifests passed.')
  await checkBuiltWorkers()
}

main().catch((error) => {
  console.error(error)
  process.exitCode = 1
}).finally(() => fs.rmSync(fixture, { recursive: true, force: true, maxRetries: 4 }))

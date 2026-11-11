import { existsSync } from 'fs'
import { join } from 'path'
import { Worker } from 'worker_threads'
import { ConfigService } from './config'
import { decryptDatWithAddon, encryptDatWithAddon, resolveNativeAddonLocation, shouldEnableNative, type NativeDatMeta } from './imageNativeAddon'

export type { NativeDatMeta } from './imageNativeAddon'

function getConfiguredAddonPath(): string | null {
  try {
    return String(new ConfigService().get('imageNativeAddonPath') || '').trim() || null
  } catch {
    return null
  }
}

export function nativeAddonLocation(): string | null {
  return resolveNativeAddonLocation(getConfiguredAddonPath())
}

export function decryptDatViaNative(inputPath: string, xorKey: number, aesKey?: string) {
  return decryptDatWithAddon(getConfiguredAddonPath(), inputPath, xorKey, aesKey)
}

export function encryptDatViaNative(inputPath: string, xorKey: number, aesKey?: string, meta?: NativeDatMeta) {
  return encryptDatWithAddon(getConfiguredAddonPath(), inputPath, xorKey, aesKey, meta)
}

// ─── Worker 化解密：原生解密是同步 CPU/IO 调用，滚动时批量触发会阻塞主线程 ───

type WorkerDecryptResult = { data: Buffer; ext: string; isWxgf: boolean; meta: NativeDatMeta } | null

type PendingJob = {
  resolve: (value: WorkerDecryptResult) => void
}

let decryptWorker: Worker | null = null
let workerFailedPermanently = false
let workerJobSeq = 0
const pendingJobs = new Map<number, PendingJob>()

function resolveWorkerPath(): string | null {
  const isDev = process.env.NODE_ENV === 'development'
  const candidates = isDev
    ? [join(__dirname, '../dist-electron/imageDecryptWorker.js'), join(__dirname, 'imageDecryptWorker.js')]
    : [join(__dirname, 'imageDecryptWorker.js')]
  for (const candidate of candidates) {
    if (existsSync(candidate)) return candidate
  }
  return null
}

function flushPendingJobs(): void {
  for (const job of pendingJobs.values()) job.resolve(null)
  pendingJobs.clear()
}

function ensureDecryptWorker(): Worker | null {
  if (workerFailedPermanently) return null
  if (decryptWorker) return decryptWorker

  const workerPath = resolveWorkerPath()
  if (!workerPath) {
    workerFailedPermanently = true
    return null
  }

  try {
    const worker = new Worker(workerPath, {
      workerData: { imageNativeAddonPath: nativeAddonLocation() || '' }
    })
    worker.on('message', (msg: { id: number; ok: boolean; addonMissing?: boolean; data?: ArrayBuffer; ext?: string; isWxgf?: boolean; meta?: NativeDatMeta }) => {
      const job = pendingJobs.get(msg.id)
      if (!job) return
      pendingJobs.delete(msg.id)
      if (msg.addonMissing) {
        // worker 中找不到原生模块：停用 worker 路径，后续直接走主线程同步逻辑
        workerFailedPermanently = true
        terminateDecryptWorker()
        job.resolve(null)
        return
      }
      if (!msg.ok || !msg.data) {
        job.resolve(null)
        return
      }
      job.resolve({
        data: Buffer.from(msg.data),
        ext: msg.ext || '',
        isWxgf: Boolean(msg.isWxgf),
        meta: msg.meta || {}
      })
    })
    const handleWorkerGone = () => {
      flushPendingJobs()
      decryptWorker = null
    }
    worker.on('error', handleWorkerGone)
    worker.on('exit', handleWorkerGone)
    worker.unref()
    decryptWorker = worker
    return worker
  } catch {
    workerFailedPermanently = true
    return null
  }
}

/**
 * 在 worker 线程中执行原生 DAT 解密，不阻塞主线程。
 * worker 不可用时回退为主线程同步解密（行为与旧版一致）。
 */
export async function decryptDatViaNativeAsync(
  inputPath: string,
  xorKey: number,
  aesKey?: string
): Promise<WorkerDecryptResult> {
  if (!shouldEnableNative()) return null

  const worker = ensureDecryptWorker()
  if (!worker) {
    return decryptDatViaNative(inputPath, xorKey, aesKey)
  }

  const id = ++workerJobSeq
  return new Promise<WorkerDecryptResult>((resolve) => {
    pendingJobs.set(id, { resolve })
    try {
      worker.postMessage({ id, datPath: inputPath, xorKey, aesKey })
    } catch {
      pendingJobs.delete(id)
      resolve(decryptDatViaNative(inputPath, xorKey, aesKey))
    }
  })
}

export function terminateDecryptWorker(): void {
  if (decryptWorker) {
    void decryptWorker.terminate()
    decryptWorker = null
  }
  flushPendingJobs()
}

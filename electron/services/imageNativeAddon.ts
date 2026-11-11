import { existsSync } from 'fs'
import { bundledWindowsComponent } from './bundledComponents'

type NativeDecryptResult = {
  data: Buffer
  ext: string
  isWxgf?: boolean
  is_wxgf?: boolean
  version?: number
  aesSize?: number
  aes_size?: number
  xorSize?: number
  xor_size?: number
  rawSize?: number
  raw_size?: number
  flag?: number
}

export type NativeDatMeta = {
  version?: number
  aesSize?: number
  aes_size?: number
  xorSize?: number
  xor_size?: number
  rawSize?: number
  raw_size?: number
  flag?: number
}

type NativeAddon = {
  decryptDatNative: (inputPath: string, xorKey: number, aesKey?: string) => NativeDecryptResult
  encryptDatNative?: (inputPath: string, xorKey: number, aesKey?: string, meta?: NativeDatMeta) => Buffer
}

export function shouldEnableNative(): boolean {
  return process.env.WEFLOW_IMAGE_NATIVE !== '0'
}

function expandAsarCandidates(filePath: string): string[] {
  if (!filePath.includes('app.asar') || filePath.includes('app.asar.unpacked')) {
    return [filePath]
  }
  return [filePath.replace('app.asar', 'app.asar.unpacked'), filePath]
}

function getAddonCandidates(configuredPath: string | null): string[] {
  const configured = configuredPath || bundledWindowsComponent('image-decrypt.node')
  if (!configured) return []
  return Array.from(new Set(expandAsarCandidates(configured)))
}

function loadAddon(configuredPath: string | null): NativeAddon | null {
  if (!shouldEnableNative()) return null

  for (const candidate of getAddonCandidates(configuredPath)) {
    if (!existsSync(candidate)) continue
    try {
      // eslint-disable-next-line @typescript-eslint/no-var-requires
      const addon = require(candidate) as NativeAddon
      if (addon && typeof addon.decryptDatNative === 'function') {
        return addon
      }
    } catch {
      // try next candidate
    }
  }

  return null
}

export function resolveNativeAddonLocation(configuredPath: string | null): string | null {
  for (const candidate of getAddonCandidates(configuredPath)) {
    if (existsSync(candidate)) return candidate
  }
  return null
}

export function decryptDatWithAddon(
  configuredPath: string | null,
  inputPath: string,
  xorKey: number,
  aesKey?: string
): { data: Buffer; ext: string; isWxgf: boolean; meta: NativeDatMeta } | null {
  const addon = loadAddon(configuredPath)
  if (!addon) return null

  try {
    const result = addon.decryptDatNative(inputPath, xorKey, aesKey)
    const isWxgf = Boolean(result?.isWxgf ?? result?.is_wxgf)
    if (!result || !Buffer.isBuffer(result.data)) return null
    const rawExt = typeof result.ext === 'string' && result.ext.trim()
      ? result.ext.trim().toLowerCase()
      : ''
    const ext = rawExt ? (rawExt.startsWith('.') ? rawExt : `.${rawExt}`) : ''
    const meta: NativeDatMeta = {
      version: result.version,
      aes_size: result.aes_size ?? result.aesSize,
      xor_size: result.xor_size ?? result.xorSize,
      raw_size: result.raw_size ?? result.rawSize,
      flag: result.flag
    }
    return { data: result.data, ext, isWxgf, meta }
  } catch {
    return null
  }
}

export function encryptDatWithAddon(
  configuredPath: string | null,
  inputPath: string,
  xorKey: number,
  aesKey?: string,
  meta?: NativeDatMeta
): Buffer | null {
  const addon = loadAddon(configuredPath)
  if (!addon || typeof addon.encryptDatNative !== 'function') return null

  try {
    const result = addon.encryptDatNative(inputPath, xorKey, aesKey, meta)
    return Buffer.isBuffer(result) ? result : null
  } catch {
    return null
  }
}

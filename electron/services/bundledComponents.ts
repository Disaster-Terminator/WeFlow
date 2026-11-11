import { existsSync } from 'fs'
import { join } from 'path'

export function bundledWindowsComponent(filename: string, resourcesPath?: string | null): string | null {
  if (process.platform !== 'win32' || process.arch !== 'x64') return null
  const roots = [
    resourcesPath,
    process.resourcesPath && join(process.resourcesPath, 'resources'),
    join(__dirname, '..', 'resources')
  ]
  for (const root of roots) {
    if (!root) continue
    const candidate = join(root, 'native', 'win32-x64', filename)
    if (existsSync(candidate)) return candidate
  }
  return null
}

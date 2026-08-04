type CacheRecord<T> = {
  key: string
  storedAt: number
  expiresAt: number
  value: T
}

const DATABASE_NAME = 'arrakis-finnhub-cache'
const STORE_NAME = 'responses'

function openCache(): Promise<IDBDatabase | null> {
  if (!('indexedDB' in window)) return Promise.resolve(null)
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DATABASE_NAME, 1)
    request.onupgradeneeded = () => {
      if (!request.result.objectStoreNames.contains(STORE_NAME)) {
        request.result.createObjectStore(STORE_NAME, { keyPath: 'key' })
      }
    }
    request.onsuccess = () => resolve(request.result)
    request.onerror = () => reject(request.error)
  })
}

export async function readCache<T>(key: string): Promise<CacheRecord<T> | null> {
  try {
    const database = await openCache()
    if (!database) return null
    return await new Promise((resolve, reject) => {
      const request = database.transaction(STORE_NAME, 'readonly').objectStore(STORE_NAME).get(key)
      request.onsuccess = () => resolve((request.result as CacheRecord<T> | undefined) ?? null)
      request.onerror = () => reject(request.error)
    })
  } catch {
    return null
  }
}

export async function writeCache<T>(key: string, value: T, ttlMs: number) {
  try {
    const database = await openCache()
    if (!database) return
    const record: CacheRecord<T> = {
      key,
      storedAt: Date.now(),
      expiresAt: Date.now() + ttlMs,
      value,
    }
    await new Promise<void>((resolve, reject) => {
      const request = database.transaction(STORE_NAME, 'readwrite').objectStore(STORE_NAME).put(record)
      request.onsuccess = () => resolve()
      request.onerror = () => reject(request.error)
    })
  } catch {
    // IndexedDB is an optimization. A blocked cache must not block market data.
  }
}

export function cacheIsFresh(record: CacheRecord<unknown>) {
  return record.expiresAt > Date.now()
}

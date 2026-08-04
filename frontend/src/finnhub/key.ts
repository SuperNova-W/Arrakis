const SESSION_KEY = 'arrakis.finnhub.apiKey'

export function configuredFinnhubKey() {
  return (import.meta.env.VITE_FINNHUB_API_KEY?.trim() || sessionStorage.getItem(SESSION_KEY) || '').trim()
}

export function saveFinnhubKey(apiKey: string) {
  const normalized = apiKey.trim()
  if (normalized) sessionStorage.setItem(SESSION_KEY, normalized)
  else sessionStorage.removeItem(SESSION_KEY)
}

export function clearFinnhubKey() {
  sessionStorage.removeItem(SESSION_KEY)
}

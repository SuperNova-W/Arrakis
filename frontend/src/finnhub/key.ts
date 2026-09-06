export function configuredFinnhubKey() {
  return (import.meta.env.VITE_FINNHUB_API_KEY ?? '').trim()
}

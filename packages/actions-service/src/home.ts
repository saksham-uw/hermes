const UPSTREAM_LIMIT = 64 * 1024;

export type HomeWeather = {
  location: string;
  temperatureC: number;
  condition: string;
  highC: number;
  lowC: number;
  precipitationPercent: number;
  observedAt: number;
  source: "Open-Meteo";
};

export type HomeQuote = {
  symbol: string;
  priceUsd: number;
  changePercent: number;
};

type CacheEntry<T> = { expiresAt: number; value: T };

export class HomeDataError extends Error {}

function finite(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new HomeDataError(`Invalid ${label} from data provider`);
  }
  return value;
}

function conditionFor(code: number): string {
  if (code === 0) return "CLEAR";
  if (code <= 3) return "PARTLY CLOUDY";
  if (code === 45 || code === 48) return "FOG";
  if (code >= 51 && code <= 67) return "RAIN";
  if (code >= 71 && code <= 77) return "SNOW";
  if (code >= 80 && code <= 82) return "SHOWERS";
  if (code >= 85 && code <= 86) return "SNOW SHOWERS";
  if (code >= 95) return "THUNDERSTORM";
  return "MIXED";
}

export class HomeDataService {
  private readonly cache = new Map<string, CacheEntry<unknown>>();

  constructor(
    private readonly fetchImpl: typeof fetch = fetch,
    private readonly now: () => number = Date.now,
  ) {}

  private async cached<T>(key: string, ttlMs: number, load: () => Promise<T>): Promise<T> {
    const found = this.cache.get(key) as CacheEntry<T> | undefined;
    if (found && found.expiresAt > this.now()) return found.value;
    const value = await load();
    this.cache.set(key, { expiresAt: this.now() + ttlMs, value });
    return value;
  }

  private async json(url: string, headers?: Record<string, string>): Promise<unknown> {
    const response = await this.fetchImpl(url, {
      headers: { Accept: "application/json", ...headers },
      signal: AbortSignal.timeout(10_000),
    });
    if (!response.ok) throw new HomeDataError(`Data provider returned ${response.status}`);
    const text = await response.text();
    if (text.length > UPSTREAM_LIMIT) throw new HomeDataError("Data provider response is too large");
    try {
      return JSON.parse(text) as unknown;
    } catch {
      throw new HomeDataError("Data provider returned invalid JSON");
    }
  }

  weather(): Promise<HomeWeather> {
    return this.cached("weather", 15 * 60_000, async () => {
      const raw = (await this.json(
        "https://api.open-meteo.com/v1/forecast?latitude=43.6532&longitude=-79.3832&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max&timezone=America%2FToronto&forecast_days=1",
      )) as Record<string, any>;
      return {
        location: "TORONTO",
        temperatureC: finite(raw.current?.temperature_2m, "temperature"),
        condition: conditionFor(finite(raw.current?.weather_code, "weather code")),
        highC: finite(raw.daily?.temperature_2m_max?.[0], "daily high"),
        lowC: finite(raw.daily?.temperature_2m_min?.[0], "daily low"),
        precipitationPercent: finite(
          raw.daily?.precipitation_probability_max?.[0],
          "precipitation chance",
        ),
        observedAt: Math.floor(this.now() / 1000),
        source: "Open-Meteo",
      };
    });
  }

  crypto(): Promise<{ quotes: HomeQuote[]; observedAt: number; status: "LIVE" }> {
    return this.cached("crypto", 60_000, async () => {
      const raw = (await this.json(
        "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,ethereum,solana&vs_currencies=usd&include_24hr_change=true",
      )) as Record<string, any>;
      const configured = [
        ["BTC", "bitcoin"],
        ["ETH", "ethereum"],
        ["SOL", "solana"],
      ] as const;
      return {
        quotes: configured.map(([symbol, id]) => ({
          symbol,
          priceUsd: finite(raw[id]?.usd, `${symbol} price`),
          changePercent: finite(raw[id]?.usd_24h_change, `${symbol} change`),
        })),
        observedAt: Math.floor(this.now() / 1000),
        status: "LIVE" as const,
      };
    });
  }

  stocks(): Promise<{ quotes: HomeQuote[]; observedAt: number; status: "DELAYED" }> {
    return this.cached("stocks", 60_000, async () => {
      const symbols = ["AAPL", "NVDA", "TSLA", "SPY"];
      const quotes = await Promise.all(
        symbols.map(async (symbol): Promise<HomeQuote> => {
          const raw = (await this.json(
            `https://query1.finance.yahoo.com/v8/finance/chart/${symbol}?interval=1d&range=2d`,
            { "User-Agent": "Mozilla/5.0" },
          )) as Record<string, any>;
          const meta = raw.chart?.result?.[0]?.meta;
          return {
            symbol,
            priceUsd: finite(meta?.regularMarketPrice, `${symbol} price`),
            changePercent: finite(meta?.regularMarketChangePercent, `${symbol} change`),
          };
        }),
      );
      return {
        quotes,
        observedAt: Math.floor(this.now() / 1000),
        status: "DELAYED" as const,
      };
    });
  }
}

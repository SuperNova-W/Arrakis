# Local Kafka transport benchmark — 2026-07-29

## Result

Across three clean-topic trials of 100,000 measured events each, Arrakis processed between
340,795 and 441,354 events per second. The median trial processed 397,950 events per second with
32 ms p95 latency. The worst p95 across the three trials was 42 ms, and no delivery failures were
observed.

A separate 10,000-event smoke trial with a manual offset commit requested for every consumed event
processed 254,482 events per second at 31 ms p95 latency with no delivery failures. This smaller
trial is recorded for diagnostics and is not the headline capacity result.

## Environment

- MacBook Air, Apple M5, 10 CPU cores, 24 GB memory
- macOS 26.3
- C++20 release build
- Apache Kafka 3.8.0 in one local Docker container
- One broker, six partitions, replication factor one
- One producer and one consumer
- Eighteen ETF symbol keys distributed across the topic
- Approximately 171-byte Protocol Buffer trade events
- Idempotent Kafka producer, `acks=all`, Zstandard compression

## Method

Each trial used a new Kafka topic to prevent old records from contaminating latency measurements.
The consumer group was warmed with 5,000 events before timing began. The measured workload then
serialized 100,000 trade events with the production Protocol Buffer path, published them through
the production Kafka wrapper, consumed and deserialized them, and recorded event latency using the
producer timestamp. Throughput spans the first measured publish through the last measured consume.
Percentiles use sorted observations with linear interpolation.

The benchmark executable is `arrakis-kafka-benchmark`. Build and invoke it with:

```bash
cmake --build --preset release --target arrakis-kafka-benchmark
./build/release/arrakis-kafka-benchmark \
  --brokers localhost:19092 \
  --topic <fresh-six-partition-topic> \
  --events 100000 \
  --warmup 5000 \
  --no-commit
```

## Scope and resume wording

This is a Kafka transport benchmark. It includes Protocol Buffer serialization, idempotent
production, broker transport, consumption, and Protocol Buffer deserialization. It does not include
PostgreSQL persistence, feature engineering, model inference, or risk management. Do not describe
these numbers as full-pipeline latency.

Defensible resume wording:

> Benchmarked the C++20/Protocol Buffers Kafka transport layer at 340K+ events/second with no
> delivery failures and no worse than 42 ms p95 latency across three 100K-event trials using
> idempotent production, `acks=all`, Zstandard compression, and six partitions.

Raw results are stored in
`backend/benchmarks/results/kafka-local-2026-07-29.json`.

FROM debian:bookworm AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libssl3 libprotobuf32 librdkafka1 && rm -rf /var/lib/apt/lists/*
COPY build/release/arrakis-market-stream /opt/arrakis/arrakis-market-stream
COPY build/release/bar-aggregator /opt/arrakis/bar-aggregator
ENTRYPOINT ["/opt/arrakis/bar-aggregator"]

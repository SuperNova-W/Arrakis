FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config \
    libboost-json1.81-dev libssl-dev libprotobuf-dev protobuf-compiler \
    libabsl-dev librdkafka-dev libpq-dev libxgboost-dev xgboost \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build/container -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DARRAKIS_BUILD_XGBOOST_TRAINER=OFF \
    -DARRAKIS_BUILD_MARKET_API=ON \
    -DARRAKIS_BUILD_ML_RUNTIME=OFF \
    -DBUILD_TESTING=OFF \
    -DPostgreSQL_ROOT=/usr \
    && cmake --build build/container --parallel

FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl libboost-json1.81.0 libssl3 libprotobuf32 librdkafka1 libpq5 libxgboost0 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/container/market-ingestion /opt/arrakis/market-ingestion
COPY --from=builder /src/build/container/news-ingestion /opt/arrakis/news-ingestion
COPY --from=builder /src/build/container/news-enricher /opt/arrakis/news-enricher
COPY --from=builder /src/build/container/bar-aggregator /opt/arrakis/bar-aggregator
COPY --from=builder /src/build/container/market-api /opt/arrakis/market-api
COPY config /opt/arrakis/config
COPY tests/fixtures /opt/arrakis/fixtures
COPY artifacts /opt/arrakis/artifacts
COPY models /opt/arrakis/models
WORKDIR /opt/arrakis
EXPOSE 8080
HEALTHCHECK --interval=10s --timeout=3s --start-period=10s --retries=6 \
    CMD curl --fail --silent --show-error http://127.0.0.1:8080/health >/dev/null || exit 1
CMD ["/opt/arrakis/market-api"]

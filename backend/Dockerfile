FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config curl ca-certificates \
    libboost-json1.81-dev libssl-dev libprotobuf-dev protobuf-compiler \
    libabsl-dev librdkafka-dev libpq-dev libxgboost-dev xgboost \
    && rm -rf /var/lib/apt/lists/*

# ONNX Runtime has no Debian package. Without it, news_nlp/CMakeLists.txt silently
# builds without FinBERT support and FinbertSession's constructor throws at startup,
# leaving news-enricher dead and every market-api prediction returning 503.
# Version tracks the Homebrew onnxruntime used for local development.
ARG ONNXRUNTIME_VERSION=1.28.0
RUN set -eux; \
    case "$(dpkg --print-architecture)" in \
        amd64) ort_arch=x64 ;; \
        arm64) ort_arch=aarch64 ;; \
        *) echo "unsupported architecture: $(dpkg --print-architecture)" >&2; exit 1 ;; \
    esac; \
    ort_dir="onnxruntime-linux-${ort_arch}-${ONNXRUNTIME_VERSION}"; \
    curl -fsSL -o /tmp/onnxruntime.tgz \
        "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/${ort_dir}.tgz"; \
    tar -xzf /tmp/onnxruntime.tgz -C /tmp; \
    cp -a "/tmp/${ort_dir}/include/." /usr/local/include/; \
    cp -a "/tmp/${ort_dir}/lib/." /usr/local/lib/; \
    ldconfig; \
    rm -rf /tmp/onnxruntime.tgz "/tmp/${ort_dir}"

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

# The FinBERT ONNX session links against libonnxruntime, which is not packaged for Debian.
COPY --from=builder /usr/local/lib/libonnxruntime.so* /usr/local/lib/
RUN ldconfig

COPY --from=builder /src/build/container/market-ingestion /opt/arrakis/market-ingestion
COPY --from=builder /src/build/container/news-ingestion /opt/arrakis/news-ingestion
COPY --from=builder /src/build/container/news-enricher /opt/arrakis/news-enricher
COPY --from=builder /src/build/container/bar-aggregator /opt/arrakis/bar-aggregator
COPY --from=builder /src/build/container/market-api /opt/arrakis/market-api
COPY --from=builder /src/build/container/daily-bar-loader /opt/arrakis/daily-bar-loader
COPY config /opt/arrakis/config
COPY tests/fixtures /opt/arrakis/fixtures
COPY artifacts /opt/arrakis/artifacts
COPY models /opt/arrakis/models
WORKDIR /opt/arrakis
EXPOSE 8080
HEALTHCHECK --interval=10s --timeout=3s --start-period=10s --retries=6 \
    CMD curl --fail --silent --show-error http://127.0.0.1:8080/health >/dev/null || exit 1
CMD ["/opt/arrakis/market-api"]

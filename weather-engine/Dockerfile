FROM rust:1.90.0-slim-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY Cargo.toml Cargo.lock ./
COPY src ./src
RUN cargo test --release --locked -j 2 && cargo build --release --locked -j 2

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/target/release/box3-weather-engine /usr/local/bin/box3-weather-engine
USER 10001:10001
HEALTHCHECK --interval=30s --timeout=5s --start-period=60s --retries=3 \
    CMD curl --fail --silent --max-time 4 http://127.0.0.1:8790/v1/box-forecast > /dev/null || exit 1
ENTRYPOINT ["/usr/local/bin/box3-weather-engine"]
CMD ["--config", "/etc/box3-weather-engine/config.json", "--threads", "2", "serve", "--bind", "127.0.0.1:8790", "--refresh-seconds", "600"]

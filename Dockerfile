# Builder stage
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    ca-certificates \
    libssl-dev \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Build POCO from source (Prometheus support requires 1.12+)
ARG POCO_VERSION=1.15.0
WORKDIR /build
RUN git clone --depth 1 --branch poco-${POCO_VERSION}-release https://github.com/pocoproject/poco.git

WORKDIR /build/poco
RUN mkdir cmake-build && cd cmake-build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build . --target install -j$(nproc)

# Build application
WORKDIR /build/app
COPY CMakeLists.txt ./
COPY src ./src/
COPY test ./test/
RUN mkdir build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local \
    && cmake --build . -j$(nproc)

# Runner stage
FROM ubuntu:24.04 AS runner

# Copy POCO shared libraries from builder
COPY --from=builder /usr/local/lib/libPoco*.so* /usr/local/lib/

# Copy application binary
COPY --from=builder /build/app/build/poco_template_server /usr/local/bin/

ENV LD_LIBRARY_PATH=/usr/local/lib

ENV PORT=8080
ENV LOG_LEVEL=information
ENV OPENAI_API_KEY=
ENV OPENAI_API_URL=https://api.deepseek.com
ENV OPENAI_MODEL=deepseek-chat
ENV OPENAI_SYSTEM_PROMPT=
ENV OPENAI_TIMEOUT=60
ENV OPENAI_SSL_VERIFY=false
ENV CONFLUENCE_URL=
ENV CONFLUENCE_USER=
ENV CONFLUENCE_TOKEN=
ENV CONFLUENCE_API_TYPE=server
ENV CONFLUENCE_TIMEOUT=30
ENV CONFLUENCE_SSL_VERIFY=true

EXPOSE 8080

CMD ["poco_template_server"]

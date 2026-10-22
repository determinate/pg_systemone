# Dockerfile for postgres:16 with pg_systemone extension
FROM postgres:16-bullseye AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    postgresql-server-dev-16 \
    libcurl4-openssl-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy extension source files
COPY Makefile ./
COPY pg_systemone.control ./
COPY pg_systemone--1.0.sql ./
COPY src/ ./src/

# Compile and install extension into PostgreSQL
RUN make && make install

# Final image
FROM postgres:16-bullseye

# Install runtime curl library
RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Copy installed extension files from builder stage
COPY --from=builder /usr/lib/postgresql/16/lib/pg_systemone.so /usr/lib/postgresql/16/lib/
COPY --from=builder /usr/share/postgresql/16/extension/pg_systemone* /usr/share/postgresql/16/extension/

EXPOSE 5432
CMD ["postgres"]

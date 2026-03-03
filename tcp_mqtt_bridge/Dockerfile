FROM ghcr.io/home-assistant/amd64-base:latest AS builder

# Install build dependencies for Alpine
RUN apk add --no-cache \
    build-base \
    cmake \
    clang \
    git \
    curl

# Set Clang as the compiler
ENV CC=clang

COPY . /app
WORKDIR /app/build

# Build the project
RUN cmake .. && make

# Stage 2: Final Image
FROM ghcr.io/home-assistant/amd64-base:latest

# Labels help the Supervisor identify the addon
LABEL \
  io.hass.name="Cozy Life Bulb" \
  io.hass.description="Scans and configures cozy life bulbs" \
  io.hass.arch="amd64" \
  io.hass.type="addon" \
  io.hass.version="0.1.0"

# Copy the binary from the builder stage
COPY --from=builder /app/build/cozy_life_bulb /usr/local/bin/

# Copy the startup script
COPY run.sh /
RUN chmod +x /run.sh

CMD [ "/run.sh" ]
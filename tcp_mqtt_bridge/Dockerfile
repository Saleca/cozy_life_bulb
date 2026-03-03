# Stage 1: The Builder
FROM ghcr.io/home-assistant/aarch64-base:latest AS builder

# Install build tools AND the mosquitto development headers
RUN apk add --no-cache build-base cmake mosquitto-dev cjson-dev

COPY . /app
WORKDIR /app/build

# Build using CMake
RUN cmake .. && make -j1

# Stage 2: The Runner (RPi 4)
FROM ghcr.io/home-assistant/aarch64-base:latest

# IMPORTANT: You must install the library here too, or the app won't start
RUN apk add --no-cache mosquitto-libs cjson

# Copy the binary from the builder
COPY --from=builder /app/build/cozy_life_bulb /usr/local/bin/

# Copy your startup script
COPY run.sh /
RUN chmod +x /run.sh

CMD [ "/run.sh" ]
FROM ghcr.io/home-assistant/aarch64-base:latest

# 1. Install build tools
RUN apk add --no-cache build-base cmake

# 2. Copy your code and build it
COPY . /app
WORKDIR /app/build
RUN cmake .. && make

# 3. Move the finished program to a standard location
RUN cp cozy_life_bulb /usr/local/bin/

# 4. Setup the startup script
COPY run.sh /
RUN chmod +x /run.sh

CMD [ "/run.sh" ]
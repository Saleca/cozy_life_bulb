FROM ghcr.io/home-assistant/aarch64-base:latest AS builder

RUN apk add --no-cache build-base cmake mosquitto-dev cjson-dev

COPY . /app
WORKDIR /app/build

RUN cmake .. && make -j1

FROM ghcr.io/home-assistant/aarch64-base:latest

RUN apk add --no-cache mosquitto-libs cjson

COPY --from=builder /app/build/cozy_life_bulb /usr/local/bin/

COPY run.sh /
RUN chmod +x /run.sh

ENTRYPOINT [ "/init" ]
CMD [ "/run.sh" ]
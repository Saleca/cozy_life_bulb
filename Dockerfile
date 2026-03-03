FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y \
    clang-18 \
    cmake \
    make \
    git \
    curl

ENV CC=clang-18

COPY . /app
WORKDIR /app/build

RUN cmake .. && make

FROM debian:bookworm-slim
COPY --from=builder /app/build/cozy_life_bulb /usr/local/bin/
COPY run.sh /
RUN chmod +x /run.sh
CMD [ "/run.sh" ]
# Build and run the NINJAM server in a container.
#
#   docker build -f ninjam/server/Dockerfile -t ninjamsrv .     (from the repository root)
#   docker run -d -p 2049:2049 -p 2049:2049/udp ninjamsrv
#
# The UDP port is for Live (players hear each other in real time); without it,
# everyone still plays through intervals.
#
# To use your own config, mount it over /config/ninjamsrv.cfg. Session archives
# (SessionArchive /data/archive ...) and logs can go in the /data volume.

FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make && rm -rf /var/lib/apt/lists/*
COPY WDL /src/WDL
COPY ninjam /src/ninjam
RUN make -C /src/ninjam/server

FROM debian:bookworm-slim
COPY --from=build /src/ninjam/server/ninjamsrv /usr/local/bin/ninjamsrv
COPY ninjam/server/docker.cfg /config/ninjamsrv.cfg
RUN useradd --system --home-dir /data --create-home ninjam
USER ninjam
WORKDIR /data
VOLUME /data
EXPOSE 2049
EXPOSE 2049/udp
ENTRYPOINT ["ninjamsrv", "/config/ninjamsrv.cfg"]

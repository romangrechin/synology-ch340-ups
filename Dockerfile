FROM debian:12

# NUT ships no ./configure in git, so the image carries autotools plus the
# perl and python that NUT's autogen.sh uses to regenerate its helper files,
# and the cross toolchains for ARM Synology models.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential autoconf automake libtool pkg-config patch \
      gcc-aarch64-linux-gnu libc6-dev-arm64-cross \
      gcc-arm-linux-gnueabihf libc6-dev-armhf-cross \
      perl python3 python-is-python3 \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY docker/build.sh /usr/local/bin/build-spk
RUN chmod 755 /usr/local/bin/build-spk

WORKDIR /src
ENTRYPOINT ["/usr/local/bin/build-spk"]

# Old-glibc Linux build image for lmcurl release binaries: manylinux_2_28 (AlmaLinux 8, glibc 2.28),
# the oldest glibc a current maintained toolchain targets. The binary links everything but glibc statically,
# so the glibc of the BUILD image sets the floor of the hosts it runs on.
# IMAGE=quay.io/pypa/manylinux_2_28_aarch64 for arm64.
ARG IMAGE=quay.io/pypa/manylinux_2_28_x86_64
FROM ${IMAGE}
RUN dnf install -y perl-core xz curl ca-certificates python3 make && dnf clean all
# Node (to run the pinned lmscript compiler the tests use), checksum-verified.
RUN case "$(uname -m)" in x86_64) N=x64;; aarch64) N=arm64;; *) exit 1;; esac \
 && cd /tmp && curl -fsSLO https://nodejs.org/dist/latest-v24.x/SHASUMS256.txt \
 && F=$(grep "linux-$N.tar.xz" SHASUMS256.txt | awk '{print $2}') && curl -fsSLO "https://nodejs.org/dist/latest-v24.x/$F" \
 && grep " $F\$" SHASUMS256.txt | sha256sum --check --strict && tar -xJf "$F" -C /usr/local --strip-components=1 && rm -f "$F" SHASUMS256.txt
# The tests use a newer Python than the image's system 3.6, and the arm64 image carries no ninja.
ENV PATH=/opt/python/cp312-cp312/bin:$PATH HOME=/tmp
RUN command -v ninja >/dev/null || python3 -m pip install --no-cache-dir ninja
RUN npm install --global @lmctl-ai/lmctl@0.2.482
WORKDIR /work/source

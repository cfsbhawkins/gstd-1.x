FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies
RUN apt-get update && apt-get install -y \
    automake \
    libtool \
    pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libglib2.0-dev \
    libjson-glib-dev \
    gtk-doc-tools \
    libedit-dev \
    libncursesw5-dev \
    libdaemon-dev \
    libjansson-dev \
    libsoup-3.0-dev \
    python3-pip \
    python3-setuptools \
    ninja-build \
    meson \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /gstd

# Copy source code
COPY . .

# Default: build with meson and run tests
CMD ["sh", "-c", "meson setup build --wipe 2>/dev/null || meson setup build && ninja -C build && ninja -C build test"]

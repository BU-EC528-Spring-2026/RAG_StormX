FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# 1. Install the Heavy Dependencies (Isolated inside this image)
# We include 'gdb' for debugging and 'vim' just in case you need to edit inside.
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libboost-all-dev \
    libomp-dev \
    libtbb-dev \
    swig \
    python3 \
    python3-pip \
    python3-dev \
    gdb \
    wget \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install numpy

WORKDIR /workspace

CMD ["/bin/bash"]

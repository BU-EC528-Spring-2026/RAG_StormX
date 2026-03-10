FROM mcr.microsoft.com/oss/mirror/docker.io/library/ubuntu:20.04
WORKDIR /app

ENV DEBIAN_FRONTEND=noninteractive

# 1. Install base dependencies
RUN apt-get update && apt-get -y install wget build-essential swig cmake git libnuma-dev python3.8-dev python3-distutils gcc-8 g++-8 \
    libboost-filesystem-dev libboost-test-dev libboost-serialization-dev libboost-regex-dev libboost-thread-dev libboost-system-dev libtbb-dev

RUN wget https://bootstrap.pypa.io/pip/3.8/get-pip.py && python3.8 get-pip.py && python3.8 -m pip install numpy

RUN apt-get update && apt-get -y install \
    gcc-9 g++-9 libboost-all-dev 

# 2. Install Aerospike C Client (Translated from your shell trace)
RUN cd /tmp && \
    wget https://download.aerospike.com/artifacts/aerospike-client-c/7.3.0/aerospike-client-c_7.3.0_ubuntu20.04_x86_64.tgz && \
    tar -xvzf aerospike-client-c_7.3.0_ubuntu20.04_x86_64.tgz && \
    cd aerospike-client-c_7.3.0_ubuntu20.04_x86_64 && \
    dpkg -i *.deb && \
    apt-get -f -y install && \
    rm -rf /tmp/aerospike-client-c_7.3.0_ubuntu20.04_x86_64*

ENV PYTHONPATH=/app/Release

# 3. Setup compiler alternatives
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-9 90 --slave /usr/bin/g++ g++ /usr/bin/g++-9

# 4. Copy source code
COPY CMakeLists.txt ./
COPY AnnService ./AnnService/
COPY Test ./Test/
COPY Wrappers ./Wrappers/
COPY GPUSupport ./GPUSupport/
COPY ThirdParty ./ThirdParty/

# 5. Build
RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-9 -DCMAKE_C_COMPILER=gcc-9 .. && \
    make -j$(nproc --ignore=1)
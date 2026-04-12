FROM mcr.microsoft.com/oss/mirror/docker.io/library/ubuntu:20.04
WORKDIR /app

ENV DEBIAN_FRONTEND=noninteractive

# 1. Install base dependencies
RUN apt-get update && apt-get -y install wget build-essential swig cmake git libnuma-dev python3.8-dev python3-distutils gcc-8 g++-8 \
    libboost-filesystem-dev libboost-test-dev libboost-serialization-dev libboost-regex-dev libboost-thread-dev libboost-system-dev libtbb-dev \
    pkg-config protobuf-compiler libprotobuf-dev libgrpc++-dev protobuf-compiler-grpc

RUN wget https://bootstrap.pypa.io/pip/3.8/get-pip.py && python3.8 get-pip.py && python3.8 -m pip install numpy

# 2. Install official Aerospike C Client prerequisites
RUN apt-get update && apt-get -y install \
    gcc-9 g++-9 libboost-all-dev \
    libssl-dev libyaml-dev zlib1g-dev \
    libev-dev libevent-dev libuv1-dev libc6-dev \
    libsnappy-dev liblz4-dev libzstd-dev

# 2b. Install newer RocksDB (Ubuntu 20.04 package is too old)
RUN apt-get update && apt-get -y install ca-certificates build-essential && \
    git clone --depth 1 --branch v8.11.3 https://github.com/facebook/rocksdb.git /tmp/rocksdb && \
    cmake -S /tmp/rocksdb -B /tmp/rocksdb/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DWITH_GFLAGS=OFF -DWITH_TESTS=OFF -DWITH_TOOLS=OFF -DWITH_BENCHMARK_TOOLS=OFF \
        -DWITH_TBB=OFF \
        -DWITH_SNAPPY=OFF -DWITH_LZ4=OFF -DWITH_ZLIB=OFF -DWITH_ZSTD=OFF \
        -DWITH_JEMALLOC=OFF -DWITH_BZ2=OFF -DWITH_NUMA=OFF \
        -DROCKSDB_BUILD_SHARED=OFF -DWITH_CORE_TOOLS=OFF -DWITH_RUNTIME_DEBUG=OFF \
        -DUSE_RTTI=1 && \
    cmake --build /tmp/rocksdb/build -j"$(nproc)" && \
    cmake --install /tmp/rocksdb/build && \
    rm -rf /tmp/rocksdb

# 3. Install Aerospike C Client
RUN cd /tmp && \
    wget https://download.aerospike.com/artifacts/aerospike-client-c/7.3.0/aerospike-client-c_7.3.0_ubuntu20.04_x86_64.tgz && \
    tar -xvzf aerospike-client-c_7.3.0_ubuntu20.04_x86_64.tgz && \
    cd aerospike-client-c_7.3.0_ubuntu20.04_x86_64 && \
    dpkg -i *.deb && \
    apt-get -f -y install && \
    rm -rf /tmp/aerospike-client-c_7.3.0_ubuntu20.04_x86_64*

ENV PYTHONPATH=/app/Release

# 4. Setup compiler alternatives
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-9 90 --slave /usr/bin/g++ g++ /usr/bin/g++-9

# 5. Copy source code
COPY CMakeLists.txt ./
COPY AnnService ./AnnService/
COPY Test ./Test/
COPY Wrappers ./Wrappers/
COPY GPUSupport ./GPUSupport/
COPY ThirdParty ./ThirdParty/

# 6. Build
RUN cd /app/ThirdParty/kvproto && ./generate_cpp.sh && \
    mkdir /app/build && cd /app/build && \
    AS_INC=$(find /usr /opt /lib -name "aerospike.h" 2>/dev/null | grep "aerospike/aerospike.h" | head -n 1 | sed 's|/aerospike/aerospike.h||') && \
    AS_LIB=$(find /usr /opt /lib -name "libaerospike.so" 2>/dev/null | head -n 1) && \
    if [ -z "$AS_LIB" ]; then AS_LIB=$(find /usr /opt /lib -name "libaerospike.a" 2>/dev/null | head -n 1); fi && \
    echo "--- Found Aerospike Include Dir: $AS_INC ---" && \
    echo "--- Found Aerospike Library: $AS_LIB ---" && \
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-9 -DCMAKE_C_COMPILER=gcc-9 -DAEROSPIKE=ON \
          -DROCKSDB=ON \
        -DTIKV=ON \
          -DAEROSPIKE_INCLUDE_DIR="$AS_INC" \
          -DAEROSPIKE_CLIENT_LIBRARY="$AS_LIB" .. && \
    make -j$(nproc)

# 7. Verify build artifacts exist
RUN ls /app/Release/server /app/Release/indexbuilder /app/Release/_SPTAG.so 2>/dev/null || \
    (echo "--- Build artifacts missing! ---" && ls -la /app/Release/ && exit 1)

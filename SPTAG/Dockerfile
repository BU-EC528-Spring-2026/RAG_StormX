FROM mcr.microsoft.com/oss/mirror/docker.io/library/ubuntu:20.04
WORKDIR /app

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get -y install wget build-essential swig cmake git libnuma-dev python3.8-dev python3-distutils gcc-8 g++-8 \
    libboost-filesystem-dev libboost-test-dev libboost-serialization-dev libboost-regex-dev libboost-serialization-dev libboost-regex-dev libboost-thread-dev libboost-system-dev libtbb-dev

RUN wget https://bootstrap.pypa.io/pip/3.8/get-pip.py && python3.8 get-pip.py && python3.8 -m pip install numpy

RUN apt-get update && apt-get -y install \
    wget build-essential swig cmake git libnuma-dev \
    python3.8-dev python3-distutils \
    gcc-9 g++-9 \
    libboost-all-dev libtbb-dev

ENV PYTHONPATH=/app/Release

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-9 90 --slave /usr/bin/g++ g++ /usr/bin/g++-9

COPY CMakeLists.txt ./
COPY AnnService ./AnnService/
COPY Test ./Test/
COPY Wrappers ./Wrappers/
COPY GPUSupport ./GPUSupport/
COPY ThirdParty ./ThirdParty/

RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-9 -DCMAKE_C_COMPILER=gcc-9 .. && \
    make -j$(nproc --ignore=1)

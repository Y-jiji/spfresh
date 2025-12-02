FROM mcr.microsoft.com/oss/mirror/docker.io/library/ubuntu:20.04

WORKDIR /app
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get -y install \
    wget build-essential swig cmake git \
    libboost-filesystem-dev libboost-test-dev \
    libboost-serialization-dev libboost-regex-dev \
    libboost-thread-dev libboost-system-dev \
    libjemalloc-dev libsnappy-dev libgflags-dev \
    pkg-config libtbb-dev libisal-dev \
    gcc-9 g++-9

ENV PYTHONPATH=/app/Release

COPY CMakeLists.txt ./
COPY AnnService ./AnnService/
COPY Test ./Test/
COPY Wrappers ./Wrappers/
COPY GPUSupport ./GPUSupport/
COPY ThirdParty ./ThirdParty/

# deps (often already present, but shown explicitly)
RUN apt-get update && apt-get install -y \
    cmake libjemalloc-dev libsnappy-dev libgflags-dev pkg-config \
    swig libboost-all-dev libtbb-dev libisal-dev git build-essential

# build & install modified RocksDB
RUN git clone https://github.com/PtilopsisL/rocksdb.git /opt/rocksdb \
 && cd /opt/rocksdb && mkdir build && cd build \
 && cmake -DUSE_RTTI=1 -DWITH_JEMALLOC=1 -DWITH_SNAPPY=1 \
          -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-fPIC" .. \
 && make -j"$(nproc)" && make install

# Build SPDK (into ThirdParty/spdk/build/lib/libspdk_*.a)
RUN cd ThirdParty/spdk && \
    ./scripts/pkgdep.sh && \
    CC=gcc-9 ./configure && \
    CC=gcc-9 make -j"$(nproc)"

# Build isal-l_crypto (for libisal_crypto.a under .libs/)
RUN cd ThirdParty/isal-l_crypto && \
    ./autogen.sh && ./configure && make -j"$(nproc)"

# Now build SPFresh
RUN mkdir build && cd build && cmake .. && make -j$(nproc) && cd ..

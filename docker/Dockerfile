# QuickFV container for a Linux x86-64 server (SPEC section 12).
#
# Build where there is internet access, then ship the image:
#   docker build -f docker/Dockerfile -t qfv:0.5 .
#   docker save qfv:0.5 | gzip > qfv-0.5.tar.gz          # copy to the server
#   docker load < qfv-0.5.tar.gz                          # on the server
#   apptainer build qfv.sif docker-archive://qfv-0.5.tar.gz   # if Apptainer, not Docker
#
# Run (MCP server for agents, session files under /work):
#   docker run -i --rm -v $PWD:/work -w /work qfv:0.5 mcp --work /work/qfv_session

FROM ubuntu:24.04 AS build
ARG OSS_CAD_SUITE=2026-09-25
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl ca-certificates python3 xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Reference tools: Yosys + slang, rIC3, btorsim, Verilator, aigsplit/aigtocnf, ...
RUN curl -sL https://github.com/YosysHQ/oss-cad-suite-build/releases/download/${OSS_CAD_SUITE}/oss-cad-suite-linux-x64-$(echo ${OSS_CAD_SUITE} | tr -d -).tgz \
    | tar -xz -C /opt

# Certificate checkers: lidrup-check (bounded claims), Certifaiger + kissat (proofs).
RUN git clone -q --depth 1 https://github.com/arminbiere/lidrup-check.git /opt/lidrup-check \
    && cd /opt/lidrup-check && ./configure && make lidrup-check
RUN git clone -q --depth 1 https://github.com/Froleyks/certifaiger.git /opt/certifaiger \
    && cd /opt/certifaiger && git submodule update --init --depth 1 \
    && cmake -DCMAKE_BUILD_TYPE=Release -B build -DSTATIC=ON -DCHECK=ON && cmake --build build -j

# QuickFV itself (slang, CaDiCaL, Btor2Tools, nlohmann/json are fetched by CMake).
COPY CMakeLists.txt /src/qfv/
COPY src /src/qfv/src
COPY tools_src /src/qfv/tools_src
RUN cmake -S /src/qfv -B /src/qfv/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /src/qfv/build

FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends python3 tcl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/oss-cad-suite /opt/oss-cad-suite
COPY --from=build /opt/lidrup-check/lidrup-check /opt/qfv/bin/
COPY --from=build /opt/certifaiger/build/certifaiger /opt/certifaiger/build/kissat /opt/qfv/bin/
COPY --from=build /src/qfv/build/qfv /opt/qfv/bin/
COPY tcl /opt/qfv/tcl
ENV PATH=/opt/qfv/bin:/opt/oss-cad-suite/bin:$PATH
ENTRYPOINT ["qfv"]

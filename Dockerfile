FROM ubuntu:26.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build libdrogon-dev qt6-base-dev libssl-dev \
    libsqlite3-dev libjsoncpp-dev uuid-dev zlib1g-dev libpq-dev \
    default-libmysqlclient-dev libbrotli-dev libhiredis-dev libyaml-cpp-dev \
    python3-dev python3-venv ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY server/ server/
COPY provider/ provider/
RUN cmake -S server -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel 2 \
    && python3 -m venv /opt/storage-env \
    && /opt/storage-env/bin/pip install --no-cache-dir -r provider/storage-requirements.txt

FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive \
    GLASS_STORAGE_PYTHON=/opt/storage-env/bin/python \
    GLASS_STORAGE_HELPER=/opt/glass/storage_provider.py
RUN apt-get update && apt-get install -y --no-install-recommends \
    libdrogon1t64 libqt6core6t64 libssl3t64 libsqlite3-0 python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 glass && useradd --uid 10001 --gid glass --no-create-home glass \
    && mkdir -p /data && chown glass:glass /data
COPY --from=build /src/build/glass-music-server /opt/glass/glass-music-server
COPY --from=build /opt/storage-env /opt/storage-env
COPY provider/storage_provider.py /opt/glass/storage_provider.py
COPY scripts/set-admin-password.py /opt/glass/set-admin-password.py
COPY assets /opt/glass/assets
COPY deploy/entrypoint.sh /opt/glass/entrypoint.sh
USER glass
WORKDIR /opt/glass
EXPOSE 8787
VOLUME ["/data"]
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 CMD python3 -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8787/api/v1/health', timeout=3)" || exit 1
ENTRYPOINT ["/bin/sh", "/opt/glass/entrypoint.sh"]

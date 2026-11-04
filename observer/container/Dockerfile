# The observer's container: a real GMod client (native Linux build) under the
# Steam client, on a virtual display, plus the supervisor. Built on the VPS
# from the observer/ folder:
#   docker build -t meowy/observer:latest -f container/Dockerfile .
# and run as a Pelican server (container/egg.yaml). Everything that lasts
# (Steam login, the game install, the job folder) lives in /home/container,
# Pelican's volume.

# --- 1: build the observer module for both Linux clients ---------------------
# GMod's Linux client is 32-bit on the main branch and 64-bit on x86-64.
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates git cmake make g++ g++-multilib \
 && rm -rf /var/lib/apt/lists/*
COPY CMakeLists.txt /src/CMakeLists.txt
COPY src /src/src
COPY lua /src/lua
RUN cmake -S /src -B /b64 -DCMAKE_BUILD_TYPE=Release && cmake --build /b64 \
 && cmake -S /src -B /b32 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_SHARED_LINKER_FLAGS=-m32 \
    -DCMAKE_SIZEOF_VOID_P=4 && cmake --build /b32 \
 && ls -l /b64/*.dll /b32/*.dll

# --- 2: the runtime ------------------------------------------------------------
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture i386 \
 && sed -i 's/^Components: main$/Components: main restricted universe multiverse/; s/^Components: main restricted$/Components: main restricted universe multiverse/' /etc/apt/sources.list.d/ubuntu.sources \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl tini procps xvfb x11vnc novnc websockify \
      libgl1-mesa-dri libgl1-mesa-dri:i386 libgl1:i386 libc6:i386 libstdc++6:i386 \
      libxtst6:i386 libxrandr2:i386 libxi6:i386 libsdl2-2.0-0:i386 libnss3 libnss3:i386 \
      zenity xdg-utils dbus-x11 nodejs \
 && echo steam steam/question select "I AGREE" | debconf-set-selections \
 && echo steam steam/license note '' | debconf-set-selections \
 && apt-get install -y --no-install-recommends steam-installer \
 && ln -sf /usr/games/steam /usr/local/bin/steam \
 && rm -rf /var/lib/apt/lists/*

# Pelican runs servers as the "container" user with /home/container as home.
RUN useradd -m -d /home/container -u 988 container
COPY container/start.sh /opt/observer/start.sh
COPY supervisor/supervisor.mjs /opt/observer/supervisor.mjs
COPY --from=build /b64/gmsv_meowy_observer_linux64.dll /opt/observer/
COPY --from=build /b32/gmsv_meowy_observer_linux32.dll /opt/observer/
RUN chmod +x /opt/observer/start.sh

USER container
ENV HOME=/home/container USER=container
WORKDIR /home/container
ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["/opt/observer/start.sh"]

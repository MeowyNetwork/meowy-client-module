# The observer's container: a real GMod client (native Linux build) under the
# Steam client, on a virtual display, plus the supervisor. Built on the VPS and
# run as a Pelican server (observer/container/egg.json). Everything that lasts
# (Steam login, the game install, the job folder) lives in /home/container,
# Pelican's volume.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture i386 \
 && sed -i 's/^Components: main restricted/Components: main restricted universe multiverse/' /etc/apt/sources.list.d/ubuntu.sources \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl tini xvfb x11vnc novnc websockify \
      libgl1-mesa-dri libgl1-mesa-dri:i386 libgl1:i386 libc6:i386 libstdc++6:i386 \
      libxtst6:i386 libxrandr2:i386 libxi6:i386 libsdl2-2.0-0:i386 libnss3 libnss3:i386 \
      zenity xdg-utils dbus-x11 \
      nodejs \
 && echo steam steam/question select "I AGREE" | debconf-set-selections \
 && echo steam steam/license note '' | debconf-set-selections \
 && apt-get install -y --no-install-recommends steam-installer \
 && ln -sf /usr/games/steam /usr/local/bin/steam \
 && rm -rf /var/lib/apt/lists/*

# Pelican runs servers as the "container" user with /home/container as home.
RUN useradd -m -d /home/container -u 988 container
COPY start.sh /opt/observer/start.sh
COPY supervisor.mjs /opt/observer/supervisor.mjs
COPY gmsv_meowy_observer_linux64.dll /opt/observer/gmsv_meowy_observer_linux64.dll
RUN chmod +x /opt/observer/start.sh

USER container
ENV HOME=/home/container USER=container
WORKDIR /home/container
ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["/opt/observer/start.sh"]

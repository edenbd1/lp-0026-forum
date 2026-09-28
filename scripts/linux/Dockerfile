FROM --platform=linux/amd64 ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq xvfb xdotool x11-utils x11-apps imagemagick dbus-x11 \
    libgl1 libegl1 libopengl0 libglx0 libxkbcommon0 libxkbcommon-x11-0 libfontconfig1 libfreetype6 libharfbuzz0b \
    libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 \
    libxcb-xinerama0 libxcb-xkb1 libxcb-xinput0 libxcb-glx0 libx11-xcb1 libvulkan1 libdbus-1-3 libglib2.0-0 \
    libqt6quickwidgets6 libqt6gui6 libqt6quick6 python3 sqlite3 procps ca-certificates fonts-dejavu-core >/dev/null \
    && rm -rf /var/lib/apt/lists/*

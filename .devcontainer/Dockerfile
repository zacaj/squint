FROM fedora:43

RUN dnf install -y \
    # Build tools
    gcc \
    pkgconfig \
    meson \
    ninja-build \
    # Required dependencies
    gtk3-devel \
    libX11-devel \
    # Optional X11 extension libraries
    libayatana-appindicator-gtk3-devel \
    libXdamage-devel \
    libXfixes-devel \
    libXi-devel \
    libXrandr-devel \
    libXrender-devel \
    libXext-devel \
    # Man page generation (optional)
    txt2tags \
    gzip \
    # DRI3/DMA-BUF + EGL for cross-GPU testing (libX11-devel already covers Xlib-xcb.h)
    libxcb-devel \
    mesa-libEGL-devel \
    mesa-libGL-devel \
    libdrm-devel \
    # Wayland source capture (xdg-desktop-portal ScreenCast -> PipeWire)
    pipewire-devel \
    # KMS source capture (scanout readback + source-GPU GBM/EGL blit)
    mesa-libgbm-devel \
    # Nested X server for cross-server testing
    xorg-x11-server-Xephyr \
    && dnf clean all


RUN useradd -m node 
USER node

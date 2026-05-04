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
    # Man page generation (optional)
    txt2tags \
    gzip \
    && dnf clean all

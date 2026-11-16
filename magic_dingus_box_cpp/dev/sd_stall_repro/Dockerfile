# The pisim image (dev/pisim) plus the GStreamer runtime pieces the repro
# needs: gst-launch to make a test video, libav + x264 for H.264/AAC.
ARG BASE=mdb-pisim:trixie
FROM ${BASE}
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        gstreamer1.0-tools gstreamer1.0-libav gstreamer1.0-plugins-ugly \
        gstreamer1.0-plugins-good gstreamer1.0-plugins-base \
    && rm -rf /var/lib/apt/lists/*

# Build environment for bootx64.efi (gnu-efi)
# Keep the image small and reproducible for GitHub Actions.
FROM debian:bookworm-slim

WORKDIR /build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      gnu-efi \
      binutils \
      file \
 && rm -rf /var/lib/apt/lists/*

CMD ["make"]

# Build environment for bootx64.efi (gnu-efi)
# Keep the image small and reproducible for GitHub Actions.
# Docker Hub auth (auth.docker.io) times out (504) on GitHub runners now and then, so the same
# official image is pulled from the AWS ECR Public mirror.
FROM public.ecr.aws/docker/library/debian:bookworm-slim

WORKDIR /build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      gnu-efi \
      binutils \
      file \
 && rm -rf /var/lib/apt/lists/*

CMD ["make"]
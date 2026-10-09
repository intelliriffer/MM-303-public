#!/usr/bin/env bash
# publish_image.sh - build the armhf toolchain image and push it to a registry.
#
#   tools/publish_image.sh [IMAGE_REPO]
#
# Default IMAGE_REPO = ghcr.io/intelliriffer/mm303-build:armhf-glibc231-audio
#
# Requires `docker login <registry>` first — for ghcr.io, a Personal Access Token
# with the `write:packages` scope:
#   echo "$GHCR_PAT" | docker login ghcr.io -u <user> --password-stdin
#
# build.sh pulls this image automatically when it isn't local (override the
# location with MM303_IMAGE_REPO).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DOCKERFILE=docker/Dockerfile.armhf-glibc231-audio
LOCAL_TAG=mm303:armhf-glibc231-audio
IMAGE_REPO="${1:-ghcr.io/intelliriffer/mm303-build:armhf-glibc231-audio}"

echo "Building $LOCAL_TAG ..."
docker build -f "$DOCKERFILE" -t "$LOCAL_TAG" .
echo "Tagging $LOCAL_TAG -> $IMAGE_REPO"
docker tag "$LOCAL_TAG" "$IMAGE_REPO"
echo "Pushing $IMAGE_REPO ..."
docker push "$IMAGE_REPO"
echo "Done: $IMAGE_REPO"

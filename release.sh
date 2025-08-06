#!/bin/bash

# Automated Firmware Release Script
# Usage: ./release.sh [release_notes]

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}🚀 Automated Firmware Release Script${NC}"

# Extract version from main.c
VERSION=$(grep -o 'firmware_version\[16\] = "[^"]*"' src/main.c | grep -o '"[^"]*"' | tr -d '"')

if [ -z "$VERSION" ]; then
    echo -e "${RED}❌ Error: Could not extract version from src/main.c${NC}"
    echo "Make sure you have: static char firmware_version[16] = \"X.Y.Z\";"
    exit 1
fi

# Get release notes
RELEASE_NOTES="$1"
if [ -z "$RELEASE_NOTES" ]; then
    RELEASE_NOTES="Firmware release v$VERSION - Automated build"
fi

echo -e "${YELLOW}📋 Version extracted from source: $VERSION${NC}"
echo -e "${YELLOW}📝 Release Notes: $RELEASE_NOTES${NC}"

# Verify version format (semantic versioning)
if ! echo "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo -e "${RED}❌ Error: Version must be in format X.Y.Z (e.g., 1.0.5)${NC}"
    echo "Current version in source: $VERSION"
    exit 1
fi

# Build firmware
echo -e "${BLUE}🔨 Building firmware...${NC}"
if ! pio run; then
    echo -e "${RED}❌ Error: Build failed${NC}"
    exit 1
fi

# Copy firmware with version name
FIRMWARE_FILE="firmware-$VERSION.bin"
echo -e "${BLUE}📦 Creating $FIRMWARE_FILE...${NC}"
cp .pio/build/esp32-c6-devkitc-1/firmware.bin ./$FIRMWARE_FILE

# Verify firmware file exists and get size
if [ ! -f "$FIRMWARE_FILE" ]; then
    echo -e "${RED}❌ Error: Firmware file not created${NC}"
    exit 1
fi

FILE_SIZE=$(stat -f%z "$FIRMWARE_FILE" 2>/dev/null || stat -c%s "$FIRMWARE_FILE" 2>/dev/null)
echo -e "${GREEN}✅ Firmware built successfully: ${FILE_SIZE} bytes${NC}"

# Git operations
echo -e "${BLUE}📝 Committing changes...${NC}"
git add src/main.c "$FIRMWARE_FILE"
if git commit -m "Release v$VERSION - $RELEASE_NOTES"; then
    echo -e "${GREEN}✅ Changes committed${NC}"
else
    echo -e "${YELLOW}⚠️  No changes to commit${NC}"
fi

echo -e "${BLUE}⬆️  Pushing to GitHub...${NC}"
git push origin main

# Create GitHub release
echo -e "${BLUE}🎉 Creating GitHub release...${NC}"
if gh release create "v$VERSION" "$FIRMWARE_FILE" \
    --title "Firmware v$VERSION" \
    --notes "$RELEASE_NOTES" \
    --latest; then
    echo -e "${GREEN}✅ GitHub release created${NC}"
else
    echo -e "${RED}❌ Error: Failed to create GitHub release${NC}"
    exit 1
fi

# Verify release was created
echo -e "${BLUE}🔍 Verifying release...${NC}"
gh release view "v$VERSION"

echo -e "${GREEN}✅ Release v$VERSION created successfully!${NC}"

# Get repository info for download URL
REPO_INFO=$(git config --get remote.origin.url | sed 's/.*github.com[:/]\([^.]*\).*/\1/')
echo -e "${GREEN}📥 Download URL: https://github.com/$REPO_INFO/releases/download/v$VERSION/$FIRMWARE_FILE${NC}"

echo -e "${BLUE}🎊 All done! Your ESP32 can now download v$VERSION via OTA!${NC}"
echo -e "${BLUE}📂 Version extracted from source: $VERSION${NC}"
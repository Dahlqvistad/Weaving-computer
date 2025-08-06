#!/bin/bash

# Quick version bump script
# Usage: ./bump.sh patch|minor|major [release_notes]

# Extract current version from main.c
CURRENT_VERSION=$(grep -o 'firmware_version\[16\] = "[^"]*"' src/main.c | grep -o '"[^"]*"' | tr -d '"')

if [ -z "$CURRENT_VERSION" ]; then
    echo "Error: Could not extract version from src/main.c"
    exit 1
fi

IFS='.' read -ra VERSION_PARTS <<< "$CURRENT_VERSION"

MAJOR=${VERSION_PARTS[0]}
MINOR=${VERSION_PARTS[1]}
PATCH=${VERSION_PARTS[2]}

case $1 in
    patch|p)
        PATCH=$((PATCH + 1))
        ;;
    minor|min)
        MINOR=$((MINOR + 1))
        PATCH=0
        ;;
    major|maj)
        MAJOR=$((MAJOR + 1))
        MINOR=0
        PATCH=0
        ;;
    *)
        echo "Usage: ./bump.sh patch|minor|major [release_notes]"
        echo "Current version: $CURRENT_VERSION"
        exit 1
        ;;
esac

NEW_VERSION="$MAJOR.$MINOR.$PATCH"
RELEASE_NOTES="$2"

echo "Bumping version: $CURRENT_VERSION → $NEW_VERSION"

# Update version in main.c
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS
    sed -i .bak "s/static char firmware_version\[16\] = \"[^\"]*\";/static char firmware_version[16] = \"$NEW_VERSION\";/" src/main.c
else
    # Linux
    sed -i.bak "s/static char firmware_version\[16\] = \"[^\"]*\";/static char firmware_version[16] = \"$NEW_VERSION\";/" src/main.c
fi

# Clean up backup file
rm -f src/main.c.bak

echo "Updated src/main.c with version: $NEW_VERSION"

# Run release script
./release.sh "$RELEASE_NOTES"
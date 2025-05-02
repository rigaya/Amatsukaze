#!/bin/bash
VER=$(git describe --tags)
echo "<#>$1"
echo "string version=\"$VER\";" >> "$1"
echo "#>" >> "$1" 
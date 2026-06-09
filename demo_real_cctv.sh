#!/bin/bash
# Demo with real CCTV images from Unsplash

set -e

echo "=== Real CCTV Edge Detection Demo ==="
echo ""

# Check if we have real images
if [ ! -f "cctv_real/security2.jpg" ]; then
    echo "ERROR: Real CCTV images not found in cctv_real/"
    echo "Run: cd cctv_real && wget ... (see demo_real_cctv.sh)"
    exit 1
fi

# Create working directories
mkdir -p input_real output_real

# Convert JPG to PPM for processing
echo "Converting real CCTV images to PPM format..."
for jpg in cctv_real/*.jpg; do
    [ -s "$jpg" ] || continue  # Skip empty files
    basename_no_ext=$(basename "$jpg" .jpg)

    # Use ImageMagick or ffmpeg to convert JPG → PPM
    if command -v convert &> /dev/null; then
        convert "$jpg" "input_real/${basename_no_ext}.ppm" && echo "  ✓ $basename_no_ext"
    elif command -v ffmpeg &> /dev/null; then
        ffmpeg -i "$jpg" "input_real/${basename_no_ext}.ppm" -loglevel quiet && echo "  ✓ $basename_no_ext"
    else
        echo "  ⚠ ImageMagick/ffmpeg not found - trying Python PIL..."
        python3 << EOF
from PIL import Image
import sys
img = Image.open('$jpg').convert('RGB')
img.save('input_real/${basename_no_ext}.ppm')
EOF
        echo "  ✓ $basename_no_ext"
    fi
done

echo ""
echo "Images ready for processing:"
ls -lh input_real/*.ppm

echo ""
echo "Processing with parallel config (4 procs × 2 threads)..."
time ./imgprocess --procs 4 --threads 2 input_real output_real

echo ""
echo "Creating visual comparison..."
feh --montage input_real/*.ppm --output real_cctv_before.png 2>/dev/null || echo "(feh not available)"
feh --montage output_real/*.ppm --output real_cctv_after.png 2>/dev/null || echo "(feh not available)"

echo ""
echo "✓ Real CCTV Demo Complete!"
echo ""
echo "Results:"
echo "  - real_cctv_before.png (original surveillance footage)"
echo "  - real_cctv_after.png (Sobel edge detection applied)"
echo ""
echo "Real-world value:"
echo "  • Motion detection in surveillance systems"
echo "  • Object recognition pipelines"
echo "  • Low-light footage enhancement"

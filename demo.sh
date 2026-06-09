#!/bin/bash
# Demo script: benchmark + visualize with feh montage

set -e

echo "=== Image Processing Pipeline Demo ==="
echo ""

# Step 1: Generate test images if needed
if [ ! -d "input" ] || [ $(ls input/*.ppm 2>/dev/null | wc -l) -eq 0 ]; then
    echo "Generating test images..."
    python3 gen_test_images.py 12 input
fi

# Step 2: Run processor with different configs
echo ""
echo "Running benchmarks..."
echo ""

mkdir -p output_demo

# Config 1: Single process, single thread (baseline)
echo "Config 1: 1 process × 1 thread"
./imgprocess --procs 1 --threads 1 input output_demo

# Config 2: 4 processes, 4 threads
echo ""
echo "Config 2: 4 processes × 4 threads"
./imgprocess --procs 4 --threads 4 input output_demo

# Step 3: Create visual comparisons with feh montage
echo ""
echo "Creating visual montage..."
echo ""

# Select first 6 images for montage (faster preview)
IMGS_IN=$(ls input/img000[0-5].ppm | head -6)
IMGS_OUT=$(ls output_demo/img000[0-5].ppm | head -6)

# Create before/after montage
feh --montage $IMGS_IN --output before_montage.png 2>/dev/null || echo "feh not available - skipping montage"
feh --montage $IMGS_OUT --output after_montage.png 2>/dev/null || echo "feh not available - skipping montage"

echo "✓ Demo complete!"
echo ""
echo "Files generated:"
echo "  - before_montage.png (original images)"
echo "  - after_montage.png (processed with edge detection)"
echo ""
echo "To view: feh before_montage.png after_montage.png"

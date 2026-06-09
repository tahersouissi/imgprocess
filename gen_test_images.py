#!/usr/bin/env python3
"""
gen_test_images.py — generate N synthetic PPM images for benchmarking.
Usage: python3 gen_test_images.py [N] [outdir]
Default: 100 images in ./input/
"""
import os, sys, struct, random, math

def make_ppm(path, w=256, h=256, seed=0):
    rng = random.Random(seed)
    data = bytearray(w * h * 3)
    # paint coloured circles so edge-detection has something to detect
    for _ in range(12):
        cx, cy = rng.randint(0, w-1), rng.randint(0, h-1)
        r = rng.randint(20, 60)
        col = [rng.randint(60, 255) for _ in range(3)]
        for y in range(max(0, cy-r), min(h, cy+r+1)):
            for x in range(max(0, cx-r), min(w, cx+r+1)):
                if (x-cx)**2 + (y-cy)**2 <= r*r:
                    idx = (y*w+x)*3
                    data[idx:idx+3] = col
    with open(path, 'wb') as f:
        f.write(f'P6\n{w} {h}\n255\n'.encode())
        f.write(bytes(data))

def main():
    n      = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    outdir = sys.argv[2]      if len(sys.argv) > 2 else 'input'
    os.makedirs(outdir, exist_ok=True)
    print(f'Generating {n} PPM images in {outdir}/ ...')
    for i in range(n):
        make_ppm(os.path.join(outdir, f'img{i:04d}.ppm'), seed=i)
        if (i+1) % 10 == 0:
            print(f'  {i+1}/{n}')
    print('Done.')

if __name__ == '__main__':
    main()

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
LDFLAGS = -lpthread -lm

all: imgprocess

imgprocess: imgprocess.c
	$(CC) $(CFLAGS) -o imgprocess imgprocess.c $(LDFLAGS)

clean:
	rm -f imgprocess
	rm -rf input/ output*/

test: imgprocess
	python3 gen_test_images.py 100 input
	./imgprocess --procs 1 --threads 1 input output_seq

benchmark: imgprocess
	python3 gen_test_images.py 100 input
	./imgprocess --benchmark input output

.PHONY: all clean test benchmark

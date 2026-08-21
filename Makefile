# aalib flags come from aalib-config when it is on PATH; if it is not, set them
# by hand, e.g. make AA_CFLAGS=-I/opt/aa/include AA_LIBS="-L/opt/aa/lib -laa"
AA_CFLAGS = $(shell aalib-config --cflags 2>/dev/null)
AA_LIBS   = $(shell aalib-config --libs 2>/dev/null || echo -laa)

IMAGE     = tenox7/ttyraid
PLATFORMS = linux/amd64,linux/arm64,linux/arm/v7,linux/386,linux/ppc64le,linux/s390x,linux/riscv64

CC     = cc
CFLAGS = -O2 -Wall $(AA_CFLAGS)
LIBS   = $(filter-out -lm,$(AA_LIBS)) -lm

all: ttyraid

ttyraid: ttyraid.c aatty.c aatty.h
	$(CC) $(CFLAGS) -o ttyraid ttyraid.c aatty.c $(LIBS)

run: ttyraid
	./ttyraid

# README screenshots (needs ImageMagick for the pgm -> png step)
shots: ttyraid
	./ttyraid -p title.pgm -S 100x30
	./ttyraid -p gameplay.pgm -S 100x30 -d -n 2200
	magick title.pgm title.png
	magick gameplay.pgm gameplay.png
	rm -f title.pgm gameplay.pgm

docker:
	docker build -t ttyraid .

# multi-arch straight to the registry; needs a docker-container buildx builder
push:
	docker buildx build --platform $(PLATFORMS) -t $(IMAGE) --push .

clean:
	rm -f ttyraid *.pgm
	rm -rf *.dSYM

.PHONY: all run shots docker push clean

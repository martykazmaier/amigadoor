# Spike A: builds a1200spike on the BBS box (or anywhere with libjxl.so.0.11 present at run time).
CFLAGS = -O2 -Wall -Wextra -Ijxl_include
a1200spike: a1200spike.c jxl_enc.c jxl_enc.h
	$(CC) $(CFLAGS) -o $@ a1200spike.c jxl_enc.c -ldl -Wl,--no-as-needed -lm
clean:
	rm -f a1200spike

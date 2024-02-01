all: store-latency-magic store-latency-boring

store-latency-magic: store-latency-magic.o

store-latency-boring: store-latency-boring.o

store-latency-magic.o: store-latency.c
	$(CC) $(CFLAGS) -DMAGIC_FENCE $< -o $@ -O3 -c

store-latency-boring.o: store-latency.c
	$(CC) $(CFLAGS) -UMAGIC_FENCE $< -o $@ -O3 -c

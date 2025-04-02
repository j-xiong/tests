ibv_bw: ibv_bw.c
	$(CC) $(CFLAGS) -o ibv_bw ibv_bw.c $(LDFLAGS) -libverbs

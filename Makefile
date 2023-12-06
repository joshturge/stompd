stompd: stompd.c stompd.h stomp.h
	cc -levent -g -O0 -Wall -o stompd stompd.c

all: sample

sample: sample.o lzss.o optlist.o
	gcc -o sample $<

sample.o:  sample.c lzss.h optlist.h
	gcc -c $<

lzss.o: lzss.c
	gcc -lOpenCL -lm -c $<

optlist.o: optlist.c optlist.h
	gcc -c $<

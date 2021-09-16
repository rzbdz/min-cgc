all: clean gcc_log run

debug: clean gcc_debug run

log: clean gcc_log run

test: clean gcc_test run

clean:
	rm -f *.o *.out *.i *.S *.s

gcc_debug_and_log:
	gcc cgc.c -o cgc.o -g -DDEBUG_ -DLOG_ -DTEST_ -Wall

gcc_debug:
	gcc -o cgc.o cgc.c -g -DDEBUG_  -DTEST_ -Wall

gcc_log:
	gcc -o cgc.o cgc.c -g -DLOG_ -Wall

gcc_test:
	gcc -o cgc.o cgc.c -g -DTEST_ -Wall


run:
	./cgc.o
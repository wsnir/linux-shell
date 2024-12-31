GCC_FLAGS = gcc -g -Wall

all: myshell

myshell: myshell.o LineParser.o
	$(GCC_FLAGS) -o $@ $^

%.o: %.c LineParser.h
	$(GCC_FLAGS) -o $@ -c $<

.PHONY: clean

clean:
	rm -f *.o myshell
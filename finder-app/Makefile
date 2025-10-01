writer: writer.c

ifdef CROSS_COMPILE
	$(CROSS_COMPILE)gcc -c -o writer writer.c
	$(CROSS_COMPILE)gcc -o writer writer.c
else
	gcc -c -o writer writer.c
	gcc -o writer writer.c
endif
clean:
	rm -f writer writer.o 

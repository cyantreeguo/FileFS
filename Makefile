all:
#	../../compiler/tcc/tcc main.c FileFS.c -o demo.exe
#	gcc -g main.c FileFS.c -o demo
	gcc main.c FileFS.c -o fs
clean:
	rm demo

build: tema1_par.c
	gcc tema1_par.c helpers.c -o tema1_par -lm -lpthread -Wall -Wextra
	gcc -v

clean:
	rm -rf tema1 tema1_par

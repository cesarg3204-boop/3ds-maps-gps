test: test_main.c ../source/nmea.c ../source/render.c ../source/web_gps.c ../source/nav.c
	cc -O2 -Wall -Wextra -fsanitize=address,undefined -o test_main $^ -lm -ljansson
	./test_main

@_list:
	just -l

sqlite3c:
	./configure
	make sqlite3.c

clean:
	git clean -fd

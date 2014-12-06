function pre()
{
	cp `which bash` src/bash
	cp `which bash` src/blerg
}

function fiddle()
{
	f=$1
	o=$2
	matches mnt/.original/$o `which bash`
	cat /dev/zero | head -c 128 >> mnt/$f
	matches mnt/.original/$o `which bash`
	cat /dev/zero | head -c 50000 >> mnt/$f
	matches mnt/.original/$o `which bash`
	truncate -s 3K mnt/$f
	matches mnt/.original/$o `which bash`
}

function post()
{
	fiddle bash bash
	mv mnt/blerg mnt/blerg2
	fiddle blerg2 blerg
}


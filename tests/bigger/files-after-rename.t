function pre()
{
	cp `which bash` src/bash
}

function post()
{
	mv mnt/bash mnt/bash2
	dd if=/dev/zero of=mnt/bash2 bs=4096 count=1 > /dev/null 2>/dev/null
	matches mnt/.original/bash `which bash`
	truncate -s 4K mnt/bash2
	matches mnt/.original/bash `which bash`
	truncate -s 3K mnt/bash2
	matches mnt/.original/bash `which bash`
	
}


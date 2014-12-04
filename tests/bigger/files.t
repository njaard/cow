function pre()
{
	cp `which bash` src/bash
}

function post()
{
	dd if=/dev/zero of=mnt/bash bs=4096 count=1
	matches mnt/.original/bash `which bash`
	truncate -s 4K mnt/bash
	matches mnt/.original/bash `which bash`
	truncate -s 3K mnt/bash
	matches mnt/.original/bash `which bash`
	
}


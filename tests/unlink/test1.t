function pre()
{
	echo "hello" > src/testfile
}

function post()
{
	rm -f mnt/testfile
	nofile mnt/testfile
	exists mnt/.original/testfile
	contains mnt/.original/testfile "hello"
}



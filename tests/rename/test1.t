function pre()
{
	echo "hello" > src/testfile
}

function post()
{
	nofile mnt/.original/testfile2
	contains mnt/testfile "hello"
	mv mnt/testfile mnt/testfile2
	contains mnt/testfile2 "hello"
	contains mnt/.original/testfile "hello"
	nofile mnt/.original/testfile2
	
	mv mnt/testfile2 mnt/testfile3
	nofile mnt/testfile2 mnt/testfile mnt/.original/testfile2
	contains mnt/.original/testfile "hello"
	contains mnt/testfile3 "hello"
	mv mnt/testfile3 mnt/testfile
	contains mnt/.original/testfile "hello"
	contains mnt/testfile "hello"
}



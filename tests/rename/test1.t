function pre()
{
	echo "hello" > testfile
}

function post()
{
	nofile .original/testfile2
	contains testfile "hello"
	mv testfile testfile2
	contains testfile2 "hello"
	contains .original/testfile "hello"
	nofile .original/testfile2
	
	mv testfile2 testfile3
	nofile testfile2 testfile .original/testfile2
	contains .original/testfile "hello"
	contains testfile3 "hello"
	mv testfile3 testfile
	contains .original/testfile "hello"
	contains testfile "hello"
}



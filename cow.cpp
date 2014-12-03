#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <fuse_opt.h>

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <array>
#include <map>

#include "sql.h"

std::string origin_path="/home/charles/tmp/cow_origin";

Sql db;

void put_int(std::string &to, int64_t x)
{
	to += char((x >> 8*7) & 0xff);
	to += char((x >> 8*6) & 0xff);
	to += char((x >> 8*5) & 0xff);
	to += char((x >> 8*4) & 0xff);
	to += char((x >> 8*3) & 0xff);
	to += char((x >> 8*2) & 0xff);
	to += char((x >> 8*1) & 0xff);
	to += char((x >> 0) & 0xff);
}

int64_t get_int(const std::string &to, unsigned position)
{
	int64_t val=0;
	val |= int64_t(to[position + 0]) << 8*7;
	val |= int64_t(to[position + 1]) << 8*6;
	val |= int64_t(to[position + 2]) << 8*5;
	val |= int64_t(to[position + 3]) << 8*4;
	val |= int64_t(to[position + 4]) << 8*3;
	val |= int64_t(to[position + 5]) << 8*2;
	val |= int64_t(to[position + 6]) << 8*1;
	val |= int64_t(to[position + 7]) << 8*0;
	return val;
}



static std::string serialize_stat(struct stat &st)
{
	std::string o;
	put_int(o, st.st_mode);
	put_int(o, st.st_nlink);
	put_int(o, st.st_uid);
	put_int(o, st.st_gid);
	put_int(o, st.st_rdev);
	put_int(o, st.st_size);
	put_int(o, st.st_blocks);
	put_int(o, st.st_atime);
	put_int(o, st.st_mtime);
	put_int(o, st.st_ctime);
	return o;
}

static struct stat deserialize_stat(const std::string &x)
{
	struct stat st;
	st.st_mode = get_int(x, 0);
	st.st_nlink = get_int(x, 1);
	st.st_uid = get_int(x, 2);
	st.st_gid = get_int(x, 3);
	st.st_rdev = get_int(x, 4);
	st.st_size = get_int(x, 5);
	st.st_blocks = get_int(x, 6);
	st.st_atime = get_int(x, 7);
	st.st_mtime = get_int(x, 8);
	st.st_ctime = get_int(x, 9);

	return st;
}

class tx
{
	Sql &db;
	bool done=false;
public:
	tx(Sql &db)
		: db(db)
	{
		db.exec("savepoint sp");
	}
	
	~tx()
	{
		if (!done)
			db.exec("release sp");
	}
	
	void rollback()
	{
		if (!done)
		{
			db.exec("rollback to sp");
			done=true;
		}
	}
};

static const char dotOriginal[] = "/.original";
static bool is_original(const char *path)
{
	if (memcmp(path, dotOriginal, sizeof(dotOriginal)-2)==0)
	{
		const char back = path[sizeof(dotOriginal)-1];
		if (back == '\0')
			return true;
		if (back == '/')
			return true;
	}
	return false;
}

static const char dotCow[] = "/.cow";
static bool is_dotcow(const char *path)
{
	if (memcmp(path, dotCow, sizeof(dotCow)-2)==0)
	{
		const char back = path[sizeof(dotCow)-1];
		if (back == '\0')
			return true;
		if (back == '/')
			return true;
	}
	return false;
}


static int cow_getattr(const char *path, struct stat *stbuf)
{
	memset(stbuf, 0, sizeof(struct stat));
	if (is_dotcow(path))
		return -ENOENT;
	
	if (is_original(path))
	{
		// first, let's see if the historical data has this path
		
		if (strcmp(path, dotOriginal)==0)
			path = "/";
		else
			path = path+sizeof(dotOriginal)-1;
		
		try
		{
			std::tuple<std::string, std::string> r = 
				db.statement("select command,data from historical_files where path=?")
				.arg(path)
				.execTypes<std::string, std::string>();
			
			const std::string& type = std::get<0>(r);
			
			if (type == "rename")
			{
				const std::string& newname = std::get<1>(r);
				int res = ::stat( (origin_path + newname).c_str(), stbuf);
				if (res == -1)
					return -errno;
				return 0;
			}
			else
			{
				const struct stat st = deserialize_stat(std::get<1>(r));
				
				*stbuf = st;
				
				if (type == "rmdir")
				{
					stbuf->st_mode = S_IFDIR;
				}
				else if (type == "erased")
				{
					stbuf->st_mode = S_IFREG;
				}
				else if (type == "erased_link")
				{
					stbuf->st_mode = S_IFLNK;
				}
				
				stbuf->st_size = db.statement("select length(data)+offset from historical_filedata where path=? order by offset desc limit 1")
					.arg(path)
					.execValue<size_t>();
			}
			return 0;
		}
		catch (no_rows&)
		{
			// maybe the file is still there
			int res = ::stat( (origin_path + path).c_str(), stbuf);
			if (res == -1)
				return -errno;
			
			// but we gotta make sure it's not a new file
			unsigned c
				= db.statement("select count(*) from new_files where path=?").arg(path).execValue<unsigned>();
			if (c > 0)
				return -ENOENT;
				
			// and we gotta make sure it's not a rename
			c = db.statement("select count(*) from historical_files where data=? and command='rename'")
				.arg(path).execValue<unsigned>();
			if (c > 0)
				return -ENOENT;
			return 0;
		}
	}
	else
	{
		int res = ::stat( (origin_path + path).c_str(), stbuf);
		if (res==0)
			return 0;
		else
			return -errno;
	}
}

static int cow_opendir(const char *path, struct fuse_file_info *fi)
{
	if (is_dotcow(path))
		return -ENOENT;
	if (is_original(path))
	{
		return 0;
	}
	else
	{
		DIR *d = opendir((origin_path + path).c_str());
		if (!d)
			return -errno;
		
		fi->fh = reinterpret_cast<uint64_t>(d);
		return 0;
	}
}

static int cow_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t, struct fuse_file_info *fi)
{
	if (is_dotcow(path))
		return -ENOENT;
	if (is_original(path))
	{
		if (strcmp(path, dotOriginal)==0)
			path = "/";
		else
			path = path+sizeof(dotOriginal)-1;
		// we search for files, they must either exist in historical_files, or in the current
		// directory and not in new_files
		
		std::set<std::string> deletedPaths, newPaths;
		std::map<std::string, std::string> renamedPaths;
		
		std::string backIsMore= path;
		backIsMore.back()++;
		db
			.statement("select path from historical_files where path >=? and path <? and (command='erased' or command='rmdir')")
			.arg(path)
			.arg(backIsMore)
			.exec(Args<std::string>(), [&] (const std::tuple<std::string> &tpath)
			{
				const std::string &path = std::get<0>(tpath);
				std::string opath = path.substr(path.rfind('/')+1);
				deletedPaths.insert(opath);
			});
			
		db
			.statement("select path,data from historical_files where path >=? and path <? and (command='rename')")
			.arg(path)
			.arg(backIsMore)
			.exec(Args<std::string,std::string>(), [&] (const std::tuple<std::string,std::string> &tpath)
			{
				const std::string &path = std::get<0>(tpath);
				const std::string &newpath = std::get<1>(tpath);
				std::string opath = path.substr(path.rfind('/')+1);
				std::string onewpath = newpath.substr(newpath.rfind('/')+1);
				renamedPaths[onewpath] = opath;
			});
			
		db
			.statement("select path from new_files where path >=? and path <?  and (command='created' or command='mkdir')")
			.arg(path)
			.arg(backIsMore)
			.exec(Args<std::string>(), [&] (const std::tuple<std::string> &tpath)
			{
				const std::string &path = std::get<0>(tpath);
				std::string opath = path.substr(path.rfind('/')+1);
				newPaths.insert(opath);
			});
		
		DIR *d = opendir((origin_path + path ).c_str());
		if (d)
		{
			while (true)
			{
				dirent entry;
				dirent *has;
				readdir_r(d, &entry, &has);
				if (!has)
					break;
				if (std::strcmp(entry.d_name, "/.cow") == 0)
					continue;
				if (renamedPaths.count(entry.d_name) >0)
				{
					filler(buf, renamedPaths[entry.d_name].c_str(), nullptr, 0);
					renamedPaths.erase(entry.d_name);
				}
				else if (deletedPaths.count(entry.d_name)>0)
				{
					deletedPaths.erase(entry.d_name);
					filler(buf, entry.d_name, nullptr, 0);
				}
				else if (!newPaths.count(entry.d_name)>0)
					filler(buf, entry.d_name, nullptr, 0);
			}
			closedir(d);
		}
		for (const std::string &x : deletedPaths)
			filler(buf, x.c_str(), nullptr, 0);
		for (const std::pair<std::string,std::string> &x : renamedPaths)
			filler(buf, x.second.c_str(), nullptr, 0);
		return 0;
	}
	else
	{
		DIR *d = reinterpret_cast<DIR*>(fi->fh);
		
		while (true)
		{
			dirent entry;
			dirent *has;
			readdir_r(d, &entry, &has);
			if (!has)
				break;
			if (std::strcmp(entry.d_name, ".cow") == 0)
				continue;
			filler(buf, entry.d_name, nullptr, 0);
		}
		
		return 0;
	}
}

static int cow_releasedir(const char *path, struct fuse_file_info *fi)
{
	if (is_original(path))
	{
	
	}
	else
	{
		DIR *d = reinterpret_cast<DIR*>(fi->fh);
		closedir(d);
	}
	return 0;
}

static int cow_open(const char *path, struct fuse_file_info *fi)
{
	if (is_dotcow(path))
		return -ENOENT;
	int flags = fi->flags;
	flags &= ~O_WRONLY;
	flags &= ~O_APPEND;
	flags |= O_RDWR;
	
	if (is_original(path))
	{
		if (strcmp(path, dotOriginal)==0)
			path = "/";
		else
			path = path+sizeof(dotOriginal)-1;
		
		std::string newpath = path;
		try
		{
			newpath = 
				db.statement("select data from historical_files where path=? and command='rename'")
				.arg(path)
				.execValue<std::string>();
		}
		catch (no_rows&)
		{
			newpath = path;
		}
		
		int fd = open((origin_path + newpath).c_str(), flags);
		fi->fh = fd;
		return 0; // TODO, return error if it doesn't exist at all
	}
	
	
	int fd = open((origin_path + path).c_str(), flags);
	if (fd == -1)
		return -errno;
	fi->fh = fd;
	return 0;
}

static int cow_release(const char *path, struct fuse_file_info *fi)
{
	if (is_original(path))
	{
		if (int(fi->fh) != -1)
		close(fi->fh);
	}
	else
	{
		close(fi->fh);
	}
	return 0;
}

static int cow_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	if (is_original(path))
	{
		if (strcmp(path, dotOriginal)==0)
			path = "/";
		else
			path = path+sizeof(dotOriginal)-1;
		
		// if the data is found in historical_filedata, read it from there
		// otherwise, read from the real file
		
		const off_t startOfRead = offset;
		
		const int fd = fi->fh;
		
		while (size > 0)
		{
			const off_t startingBlock = (offset >> 12) << 12;
			
			// startingBlock is a multiple of 4096, conveniently coinciding with historical_filedata
			
			try
			{
				const std::string data = db.statement("select data from historical_filedata where path=? and offset=?")
					.arg(path)
					.arg(startingBlock)
					.execValue<std::string>();
				
				// the amount of bytes I got
				const size_t actuallyRead = data.size();
				
				{
					const size_t delta = offset-startingBlock;
					const size_t readInBlock = std::min<size_t>(size, actuallyRead-delta);
					std::memcpy(buf, data.data()+delta, readInBlock);
					
					offset += readInBlock-delta;
					size -= readInBlock;
					buf += readInBlock-delta;
				}
				
				if (actuallyRead < 4096)
					return offset - startOfRead;
			}
			catch (no_rows&)
			{
				// no overlap here, I have to satisfy this block from the real file
				if (fd == -1)
					return -EIO;

			
				// I haven't started on a 4096 block boundary
				const size_t delta = offset-startingBlock;
				const ssize_t actuallyRead = pread(fd, buf, std::min<size_t>(4096, size), startingBlock+delta);
				if (actuallyRead == -1)
					return -errno;
				
				const size_t readInBlock = std::min<size_t>(size, actuallyRead-delta);
				
				offset += readInBlock-delta;
				size -= readInBlock;
				buf += readInBlock-delta;
				
				if (actuallyRead < 4096)
					return offset - startOfRead;
			}
		}
		return offset - startOfRead;
	}
	else
	{
		ssize_t r = pread(fi->fh, buf, size, offset);
		return r;
	}
}

static int cow_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	if (is_dotcow(path))
		return -EACCES;
	int flags = fi->flags | O_CREAT | O_EXCL;
	flags &= ~O_WRONLY;
	flags &= ~O_APPEND;
	flags |= O_RDWR;
	
	int fd = ::open((origin_path + path).c_str(), flags, mode);
	if (fd == -1)
		return -errno;
	
	db.statement("insert into new_files values(?, 'create')").arg(path).exec();
	fi->fh = fd;
	return 0;
}

static void mergeData(int fd, const char *path, off_t begin, size_t bytes, size_t fsize)
{
	std::array<char, 4096> reading;
	
	off_t startingBlock = (begin >> 12) << 12;
	
	size_t lastBlockSize=0;
	
	while (size_t(startingBlock) < begin+bytes+4096 && size_t(startingBlock) < fsize)
	{
		// read the data that's being replaced
		const ssize_t r = pread(fd, reading.data(), 4096, startingBlock);
		if (r == -1)
		{
			throw std::runtime_error("failed to read: " + std::to_string(errno));
		}
		
		lastBlockSize = r;
		
		// and put what's being replaced into the historical data
		db.statement("insert or ignore into historical_filedata values(?,?,?)")
			.arg(path)
			.arg(startingBlock)
			.argBlob(reinterpret_cast<unsigned char*>(reading.data()), r)
			.exec();

		startingBlock += 4096;
	}
	
	if (lastBlockSize == 4096)
	{
		// one more empty block to indicate EOF
		db.statement("insert or ignore into historical_filedata values(?,?,?)")
			.arg(path)
			.arg(startingBlock)
			.argBlob("")
			.exec();
	}
	
}

static int cow_mkdir(const char *path, mode_t mode)
{
	if (is_dotcow(path))
		return -EACCES;
	struct stat buf;
	if (stat((origin_path + path).c_str(), &buf) == 0)
	{
		return -EEXIST;
	}
	
	tx tx(db);
	try
	{
		db.statement("insert into new_files values(?, 'mkdir')").arg(path).exec();
		
		int r = ::mkdir((origin_path + path).c_str(), mode);
		if (r == 0)
			return 0;
		tx.rollback();
		return -errno;
	}
	catch (std::exception &e)
	{
		std::cerr << "error: " << e.what() << std::endl;
		tx.rollback();
		return -EIO;
	}
}

static int cow_rmdir(const char *path)
{
	if (is_dotcow(path))
		return -ENOENT;
	struct stat buf;
	if (stat((origin_path + path).c_str(), &buf) == 0)
		return -EEXIST;
	
	tx tx(db);
	try
	{
		// is this a new dir?
		
		unsigned c = db.statement("select count(*) from new_files where path=?").arg(path).execValue<unsigned>();
		if (c == 0)
		{
			// path is historic, I have to mark it as erased
			
			db.statement("insert into historical_files values(?, 'rmdir', ?)").arg(path).argBlob(serialize_stat(buf)).exec();
		}
		else
		{
			db.statement("delete from new_files where path=?").arg(path).exec();
		}
		
		int r = ::rmdir((origin_path + path).c_str());
		if (r == 0)
			return 0;
		tx.rollback();
		return -errno;
	}
	catch (std::exception &e)
	{
		std::cerr << "error: " << e.what() << std::endl;
		tx.rollback();
		return -EIO;
	}
}


static int cow_unlink(const char *path)
{
	if (is_dotcow(path))
		return -ENOENT;
	struct stat buf;
	if (stat((origin_path + path).c_str(), &buf) == -1)
	{
		return -errno;
	}
	
	tx tx(db);
	try
	{
		// does 'path' exist right now and is it historic?
		unsigned c = db.statement("select count(*) from new_files where path=?").arg(path).execValue<unsigned>();
		if (c == 0)
		{
			// path is historic, I have to mark it as erased
			if (S_ISLNK(buf.st_mode))
			{
				struct stat sb;
				if (lstat(path, &sb) == -1)
					return -errno;
					
				std::string linkname;
				linkname.resize(sb.st_size);
				int rc = ::readlink(path, &linkname[0], linkname.length());
				if (rc == -1)
					return -errno;
				db.statement("insert into historical_files values(?, 'erased_link', ?)").arg(path).arg(linkname).exec();
			}
			else
			{
				db.statement("insert into historical_files values(?, 'erased', ?)")
					.arg(path).argBlob(serialize_stat(buf)).exec();
			}
			
			// and save its data
			const int fd = ::open( (origin_path + path).c_str(), O_RDONLY);
			
			if (fd == -1)
				return -EIO;
			
			try
			{
				mergeData(fd, path, 0, buf.st_size, buf.st_size);
			}
			catch (...)
			{
				::close(fd);
				throw;
			}
		}
		else
		{ // path is not historic, I can just forget about it
			db.statement("delete from new_files where path=?").arg(path).exec();
		}
		
		int r = ::unlink((origin_path + path).c_str());
		if (r == 0)
			return 0;
		tx.rollback();
		return -errno;
	}
	catch (std::exception &e)
	{
		std::cerr << "error: " << e.what() << std::endl;
		tx.rollback();
		return -EIO;
	}
}

static int cow_readlink(const char *path, char *buf, size_t bufsize)
{
	if (is_original(path))
	{
		if (strcmp(path, dotOriginal)==0)
			path = "/";
		else
			path = path+sizeof(dotOriginal)-1;
		
		// if the data is found in historical_filedata, read it from there
		// otherwise, read from the real file
		
		try
		{
			std::string linkpath
				= db.statement("select linkpath, from historical_files where path=? and command='erased_link'")
					.arg(path)
					.execValue<std::string>();
			
			std::memcpy(buf, linkpath.c_str(), std::min(bufsize, linkpath.length()+1));
			return 0;
		}
		catch (...)
		{
			return -EIO;
		}
	}
	else
	{
		ssize_t r = readlink((origin_path + path).c_str(), buf, bufsize);
		if (r == -1)
			return -errno;
		return 0;
	}

}

static int cow_symlink(const char *oldpath, const char *newpath)
{
	if (is_dotcow(oldpath))
		return -ENOENT;
	if (is_dotcow(newpath))
		return -EACCES;

	tx tx(db);
	
	try
	{
		db.statement("insert into new_files values(?, 'symlink')").arg(newpath).arg(oldpath).exec();
	}
	catch (std::exception &e)
	{
		std::cerr << "error: " << e.what() << std::endl;
		tx.rollback();
		return -EIO;
	}
	
	int rc = symlink(oldpath, newpath);
	if (rc == -1)
	{
		tx.rollback();
		return -errno;
	}
	return 0;
}

static int cow_rename(const char *path, const char *newpath)
{
	if (is_dotcow(path))
		return -ENOENT;
	if (is_dotcow(newpath))
		return -EACCES;
	struct stat buf;
	if (stat((origin_path + path).c_str(), &buf) == -1)
	{
		return -errno;
	}
	
	tx tx(db);
	
	try
	{
		// does 'path' exist right now and is it historic?
		unsigned c = db.statement("select count(*) from new_files where path=?").arg(path).execValue<unsigned>();
		if (c == 0)
		{
			// path is historic, I have to mark it as renamed
			
			try
			{
				std::string oldpath = db.statement("select path from historical_files where command='rename' and data=?")
					.arg(path)
					.execValue<std::string>();
				if (oldpath == path)
				{
					// if newpath is the oldpath, then it's been un-renamed
					db.statement("delete from historical_files where path=? and command='rename'").arg(oldpath).exec();
				}
				else
				{
					// it has been renamed, and it's been renamed again
					db.statement("update historical_files set data=? where path=? and command='rename'")
						.arg(newpath)
						.arg(oldpath).exec();
				
				}
			}
			catch (no_rows&)
			{
				db.statement("insert or ignore into historical_files values(?, 'rename', ?)").arg(path).arg(newpath).exec();
			}
		}
		else
		{ // path is not historic, I have to rename it
			db.statement("update new_files set path=? where path=?").arg(path).arg(newpath).exec();
		}
		
		int r = rename((origin_path + path).c_str(), (origin_path + newpath).c_str());
		if (r == -1)
		{
			tx.rollback();
			return -errno;
		}
		return 0;
	}
	catch (std::exception &e)
	{
		std::cerr << "error: " << e.what() << std::endl;
		tx.rollback();
		return -EIO;
	}
}

static int cow_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	tx tx(db);
	
	try
	{
		unsigned count = db.statement("select count(*) from new_files where path=?").arg(path).execValue<unsigned>();
		
		std::string oldpath;
		try
		{
			oldpath = 
				db.statement("select data from historical_files where data=? and command='rename'")
				.arg(path)
				.execValue<std::string>();
		}
		catch (no_rows&)
		{
			oldpath = path;
		}
		
		
		if (count == 0)
		{
			off_t pos = lseek(fi->fh, 0, SEEK_END);
			// read all the blocks from "path" that coincide with size and offset
			// only record them into historical_filedata if there's a difference
			// and it's not already in historical_filedata
			mergeData(fi->fh, oldpath.c_str(), offset, size, pos);
		}
		
		ssize_t r = pwrite(fi->fh, buf, size, offset);
		if (r == -1)
		{
			tx.rollback();
			return -errno;
		}
		return r;
	}
	catch (std::exception &e)
	{
		std::cerr << "error: " << e.what() << std::endl;
		tx.rollback();
		return -EIO;
	}
}

static int cow_truncate(const char *path, off_t len)
{
	tx tx(db);
	
	try
	{
		unsigned count = db.statement("select count(*) from new_files where path=?").arg(path).execValue<unsigned>();
		
		if (count == 0)
		{
			const int fd = ::open( (origin_path + path).c_str(), O_RDONLY);
			
			if (fd == -1)
				return -EIO;
			const off_t end = lseek(fd, 0, SEEK_END);
			
			try
			{
				mergeData(fd, path, 0, end, end);
			}
			catch (...)
			{
				::close(fd);
				throw;
			}
			::close(fd);
		}
		
		int r = truncate((origin_path + path).c_str(), len);
		if (r == -1)
		{
			tx.rollback();
			return -errno;
		}
		return 0;
	}
	catch (std::exception &e)
	{
		std::cerr << "error: " << e.what() << std::endl;
		tx.rollback();
		return -EIO;
	}

}


static int cow_fsync(const char *path, int datasync, struct fuse_file_info *)
{
	int fd = open((origin_path + path).c_str(), O_RDONLY);
	int rc;
	if (datasync)
		rc = fdatasync(fd);
	else
		rc = fsync(fd);
	close(fd);
	
	if (rc == -1)
		return -errno;
	return 0;
}

static void* cow_init(struct fuse_conn_info *)
{
	mkdir( (origin_path + "/.cow").c_str(), 0777 );
	db.open(origin_path + "/.cow/history.db");
	db.exec("create table if not exists historical_files (path primary key, command, data)");
	db.exec("create table if not exists new_files (path primary key, command)");
	db.exec("create table if not exists historical_filedata (path, offset, data, primary key (path, offset))");
	db.exec("create index if not exists historical_renames on historical_files (data,command)");
	return nullptr;
}


/*
create a file: path, 'create', mode
rename: path, 'rename', new (replaces the path of the created file)
truncate: path, 'truncate', newlen



*/
int main(int argc, char *argv[])
{
	struct fuse_operations cow_oper;
	std::memset(&cow_oper, 0, sizeof(cow_oper));
	cow_oper.getattr = cow_getattr;
	cow_oper.open = cow_open;
	cow_oper.release = cow_release;
	cow_oper.read = cow_read;
	cow_oper.write = cow_write;
	cow_oper.init = cow_init;
	cow_oper.opendir = cow_opendir;
	cow_oper.readdir = cow_readdir;
	cow_oper.releasedir = cow_releasedir;
	cow_oper.unlink = cow_unlink;
	cow_oper.mkdir = cow_mkdir;
	cow_oper.rmdir = cow_rmdir;
	cow_oper.create = cow_create;
	cow_oper.rename = cow_rename;
	cow_oper.truncate = cow_truncate;
	cow_oper.fsync = cow_fsync;
	cow_oper.symlink = cow_symlink;
	cow_oper.readlink = cow_readlink;

	char** more_argv = new char*[argc];
	more_argv[0] = argv[0];
	bool has=false;
	int at=1;
	for (int i=1; i < argc; i++, at++)
	{
		if (argv[i][0] == '-' || has)
		{
			more_argv[at] = argv[i];
			continue;
		}
		at--;
		origin_path = argv[i];
		has = true;
	}

	return fuse_main(argc-1, more_argv, &cow_oper, nullptr);
}

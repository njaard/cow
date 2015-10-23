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

std::string origin_path;
int origin_fd=-1;

Sql db;

static const char *atdir(const char *path)
{
	path++;
	if (*path == '\0')
		path = ".";
	return path;
}

static void put_int(std::vector<unsigned char> &to, uint64_t x)
{
	to.push_back( char((x >> 8*7) & 0xff) );
	to.push_back( char((x >> 8*6) & 0xff) );
	to.push_back( char((x >> 8*5) & 0xff) );
	to.push_back( char((x >> 8*4) & 0xff) );
	to.push_back( char((x >> 8*3) & 0xff) );
	to.push_back( char((x >> 8*2) & 0xff) );
	to.push_back( char((x >> 8*1) & 0xff) );
	to.push_back( char((x >> 0) & 0xff) );
}

static uint64_t get_int(const std::vector<unsigned char> &to, unsigned position)
{
	uint64_t val=0;
	val |= uint64_t(to[8*position + 0]) << 8*7;
	val |= uint64_t(to[8*position + 1]) << 8*6;
	val |= uint64_t(to[8*position + 2]) << 8*5;
	val |= uint64_t(to[8*position + 3]) << 8*4;
	val |= uint64_t(to[8*position + 4]) << 8*3;
	val |= uint64_t(to[8*position + 5]) << 8*2;
	val |= uint64_t(to[8*position + 6]) << 8*1;
	val |= uint64_t(to[8*position + 7]) << 8*0;
	return val;
}

static std::string binary_to_string(const std::vector<unsigned char> &a)
{
	return std::string( reinterpret_cast<const char*>(&a[0]), reinterpret_cast<const char*>(&a[a.size()]));
}



static std::vector<unsigned char> serialize_stat(struct stat &st)
{
	std::vector<unsigned char> o;
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

static struct stat deserialize_stat(const std::vector<unsigned char> &x)
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

struct cow_file_info
{
	int fd=-1;
	bool is_original=false;
	
	// never true on "original"-path opens:
	bool is_new; // as opposed to historical
	bool is_historical=false; // as opposed to new
	bool removed=false; // as opposed to still existing
	bool is_directory=false; // this path is a directory name
	
	std::string oldpath; // the path of this file in the historic tree (before mv)
	std::string newpath; // the path of this file in the working tree (after mv)
	
	// if not new, the command, if any
	std::string command;
	std::vector<unsigned char> commanddata;
	
	ssize_t original_file_size=-1;
	
	cow_file_info(const char *path);
	
	~cow_file_info()
	{
		if (fd != -1)
			::close(fd);
	}
	
	
	Sql& filedata()
	{
		if (!file_database.isOpen())
		{
			throw std::runtime_error("tried to get filedata on directory");
		}
		return file_database;
	}
	
	std::vector<bool> historical_blocks_present;
	
	static std::unique_ptr<cow_file_info> make(const char *path)
	{
		return std::unique_ptr<cow_file_info>(new cow_file_info(path));
	}
private:
	Sql file_database;
};

cow_file_info::cow_file_info(const char *path)
{
	if (::is_original(path))
	{
		is_original = true;
		is_new = false;
		
		if (strcmp(path, dotOriginal)==0)
			path = "/";
		else
			path = path+sizeof(dotOriginal)-1;
	}
	
	typedef Args<std::string,std::vector<unsigned char>> TwoStrings;
	
	newpath = path;
	is_historical = false;
	
	{
		for (size_t i=1; i < newpath.size(); i++)
		{
			if (newpath[i]=='/')
			{
				std::string upto = newpath.substr(0, i-1);
				::mkdirat(origin_fd, atdir(upto.c_str()), 0700);
			}
		}
	}
	
	db.statement("select command,data from historical_files where path=?")
		.arg(path)
		.exec(TwoStrings(), [&] (const TwoStrings::tuple &args)
		{
			command= std::get<0>(args);
			commanddata = std::get<1>(args);
			
			is_historical = true;
			
			if (command == "rename")
			{
				newpath = binary_to_string(commanddata);
			}
			else if (command == "erased")
			{
				newpath = "";
				removed = true;
			}
		});
	
	
	if (!is_original)
	{
		unsigned count = db.statement("select count(*) from new_files where path=?")
			.arg(newpath).execValue<unsigned>();
		is_new = (count > 0);
		newpath = path;
	}
	
	if (!is_new)
		is_historical = true; // later on, we might set this to false

	try
	{
		oldpath = 
			db.statement("select path from historical_files where data=? and command='rename'")
			.arg(path)
			.execValue<std::string>();
		is_historical = false;
	}
	catch (no_rows&)
	{
		oldpath = path;
	}
	
	if (!is_new)
	{
		// if the file is not new, then its size is:
		//   if the historical_filedata has a blocksize < 4096, then that is the last block
		//   otherwise, it's the length of the true file
		
		if (!newpath.empty())
		{
			// if I don't know the end of the file from the historical_filedata,
			// then it must be in the working dir
			struct stat stbuf;
			int res = ::fstatat( origin_fd, atdir(newpath.c_str()), &stbuf, 0);
			if (res != -1)
			{
				original_file_size = stbuf.st_size;
				is_directory = S_ISDIR(stbuf.st_mode);
			}
			else
			{
				is_historical = false;
			}
		}
		
		if (!is_directory)
		{
			file_database.open(origin_path + dotCow + "/filedata/" + oldpath);
			file_database.exec("pragma synchronous = NORMAL");
			file_database.exec("create table if not exists historical_filedata (offset integer primary key, data)");

			try
			{
				original_file_size
					= filedata().statement("select coalesce(offset+length(data),?) from historical_filedata where "
							"offset=(select coalesce(max(offset),0) from historical_filedata) "
							"and length(data)!=4096"
						)
						.arg(-1)
						.execValue<uint64_t>();
			}
			catch (no_rows&) { }
		
			// now let's gather a list of blocks that are present
			const uint64_t sz
				= file_database.statement("select coalesce(max(offset),0) from historical_filedata")
					.execValue<uint64_t>();
			
			historical_blocks_present.resize((sz / 4096)+1, false);

			file_database.statement("select offset from historical_filedata")
				.exec(Args<uint64_t>(), [this] (const std::tuple<uint64_t> &t) 
				{
					historical_blocks_present[std::get<0>(t)/4096]=true;
				});
		}
	}
}

static int cow_getattr(const char *path, struct stat *stbuf)
{
	memset(stbuf, 0, sizeof(struct stat));
	if (is_dotcow(path))
		return -ENOENT;
	
	if (is_original(path))
	{
		const std::unique_ptr<cow_file_info> info = cow_file_info::make(path);
		if (!info->is_historical)
			return -ENOENT;
		
		if (info->is_new)
			return -ENOENT;
		
		if (info->command == "rmdir" || info->command == "erased" || info->command == "erased_link")
		{
			*stbuf = deserialize_stat(info->commanddata);
		}
		else
		{
			const int res = ::fstatat( origin_fd, atdir(info->newpath.c_str()), stbuf, 0);
			if (res == -1)
			{
				std::cerr << "failed to fstat the file [origin]" << info->newpath << " which should be there" << std::endl;
				return -1;
			}
			stbuf->st_size = info->original_file_size;
		}
		return 0;
	}
	else
	{
		int res = ::fstatat( origin_fd, atdir(path), stbuf, 0);
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
		int dfd = ::openat(origin_fd, atdir(path), O_DIRECTORY);
		if (dfd == -1)
			return -errno;
		
		DIR *d = fdopendir(dfd);
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
		
		const int dfd = ::openat(origin_fd, atdir(path), O_DIRECTORY);
		if (dfd != -1)
		{
			
			DIR *d = fdopendir(dfd);
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
	
	std::unique_ptr<cow_file_info> info = cow_file_info::make(path);
	fi->fh = reinterpret_cast<int64_t>(info.get());
	
	// TODO test if this file is deleted in the working tree
	int fd = openat(origin_fd, atdir(info->newpath.c_str()), flags);
	info->fd = fd;
	info.release();
	return 0; // TODO, return error if it doesn't exist at all
}

static int cow_release(const char *, struct fuse_file_info *fi)
{
	cow_file_info *const info = reinterpret_cast<cow_file_info*>(fi->fh);
	delete info;

	return 0;
}

static int cow_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	cow_file_info *const info = reinterpret_cast<cow_file_info*>(fi->fh);
	
	if (is_original(path))
	{
		if (strcmp(path, dotOriginal)==0)
			path = "/";
		else
			path = path+sizeof(dotOriginal)-1;
		
		// if the data is found in historical_filedata, read it from there
		// otherwise, read from the real file
		
		const off_t startOfRead = offset;
		
		while (size > 0)
		{
			const off_t startingBlock = (offset >> 12) << 12;
			
			// startingBlock is a multiple of 4096, conveniently coinciding with historical_filedata
			
			try
			{
				const std::string data = info->filedata().statement("select data from historical_filedata where offset=?")
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
				if (info->fd == -1)
					return -EIO;

			
				// I haven't started on a 4096 block boundary
				const size_t delta = offset-startingBlock;
				const ssize_t actuallyRead = pread(info->fd, buf, std::min<size_t>(4096, size), startingBlock+delta);
				if (actuallyRead == -1)
					return -errno;
				
				const size_t readInBlock = std::min<size_t>(size, actuallyRead-delta);
				
				offset += readInBlock-delta;
				size -= readInBlock;
				buf += readInBlock-delta;
				
				if (actuallyRead < 4096)
					return offset - startOfRead;
			}
			catch (std::exception &e)
			{
				std::cerr << "failure: " << e.what() << std::endl;
				return -EIO;
			}
		}
		return offset - startOfRead;
	}
	else
	{
		ssize_t r = pread(info->fd, buf, size, offset);
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
	
	int fd = ::openat(origin_fd, atdir(path), flags, mode);
	if (fd == -1)
		return -errno;
	
	std::unique_ptr<cow_file_info> info = cow_file_info::make(path);
	
	fi->fh = reinterpret_cast<int64_t>(info.get());
	info->oldpath = info->newpath = path;
	info->fd = fd;
	info->is_new = true;
	info->is_original=false;
	db.statement("insert into new_files values(?, 'create')").arg(path).exec();
	info.release();
	return 0;
}

static void mergeData(
	cow_file_info *const info,
	std::vector<bool> &historical_blocks_present,
	off_t begin, size_t bytes, size_t fsize
)
{
	// it's possible that historical_blocks_present is empty incorrectly
	std::array<char, 4096> reading;
	
	size_t startingBlock = (begin >> 12) << 12;
	
	size_t lastBlockSize=0;
	
	while (startingBlock < begin+bytes+4096 && startingBlock < fsize)
	{
		// read the data that's being replaced
		
		if (historical_blocks_present.size() <= startingBlock/4096 || historical_blocks_present[startingBlock/4096])
		{
			const ssize_t r = pread(info->fd, reading.data(), 4096, startingBlock);
			if (r == -1)
			{
				throw std::runtime_error("failed to read: " + std::to_string(errno));
			}
			
			lastBlockSize = r;
			
			// and put what's being replaced into the historical data
			info->filedata().statement("insert or ignore into historical_filedata values(?,?)")
				.arg(startingBlock)
				.argBlob(reinterpret_cast<unsigned char*>(reading.data()), r)
				.exec();

			if (historical_blocks_present.size() > startingBlock/4096)
				historical_blocks_present[startingBlock/4096]=true;
		}
		else
			lastBlockSize = 4096;
			
		startingBlock += 4096;
	}
	
	if (lastBlockSize == 4096 && historical_blocks_present.size() >= startingBlock/4096)
	{
		// one more empty block to indicate EOF
		historical_blocks_present[startingBlock/4096]=true;
		info->filedata().statement("insert or ignore into historical_filedata values(?,?)")
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
	if (::fstatat(origin_fd, atdir(path), &buf, 0) == 0)
	{
		return -EEXIST;
	}
	
	tx tx(db);
	try
	{
		db.statement("insert into new_files values(?, 'mkdir')").arg(path).exec();
		
		int r = ::mkdirat(origin_fd, atdir(path), mode);
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
	if (::fstatat(origin_fd, atdir(path), &buf, 0) == 0)
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
		
		int r = ::unlinkat(origin_fd, atdir(path), AT_REMOVEDIR);
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
	if (::fstatat(origin_fd, atdir(path), &buf, 0) == -1)
	{
		return -errno;
	}
	
	std::unique_ptr<cow_file_info> info = cow_file_info::make(path);

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
			info->fd = ::openat( origin_fd, atdir(path), O_RDONLY);
			
			if (info->fd == -1)
				return -EIO;
			
			{
				// this can be empty
				std::vector<bool> historical_blocks_present;
				mergeData(info.get(), historical_blocks_present, 0, buf.st_size, buf.st_size);
			}
		}
		else
		{ // path is not historic, I can just forget about it
			db.statement("delete from new_files where path=?").arg(path).exec();
		}
		
		int r = ::unlinkat(origin_fd, atdir(path), 0);
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
		ssize_t r = readlinkat(origin_fd, atdir(path), buf, bufsize);
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
	if (::fstatat(origin_fd, atdir(path), &buf, 0) == -1)
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
				if (oldpath == newpath)
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
		
		int r = ::renameat(origin_fd, atdir(path), origin_fd, atdir(newpath));
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

static int cow_write(const char *, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	tx tx(db);
	
	cow_file_info *const info = reinterpret_cast<cow_file_info*>(fi->fh);
	
	try
	{
		if (!info->is_new)
		{
			// read all the blocks from "path" that coincide with size and offset
			// only record them into historical_filedata if there's a difference
			// and it's not already in historical_filedata
			mergeData(
				info, info->historical_blocks_present,
				offset, size, info->original_file_size);
		}
		
		ssize_t r = pwrite(info->fd, buf, size, offset);
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
	
	std::unique_ptr<cow_file_info> info = cow_file_info::make(path);
	info->fd = ::openat( origin_fd, atdir(info->newpath.c_str()), O_RDWR);
	
	if (info->fd == -1)
		return -errno;
	try
	{
		if (!info->is_new)
		{
			const off_t end = lseek(info->fd, 0, SEEK_END);
			
			std::vector<bool> historical_blocks_present;
			mergeData(info.get(), historical_blocks_present, 0, end, end);
		}
		
		int r = ftruncate(info->fd, len);
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
	int fd = ::openat(origin_fd, atdir(path), O_RDONLY);
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

	std::vector<char*> more_argv;
	more_argv.push_back(argv[0]);
	bool has=false;
	for (int i=1; i < argc; i++)
	{
		if (argv[i][0] == '-' || has)
		{
			more_argv.push_back(argv[i]);
			continue;
		}
		origin_path = argv[i];
		has = true;
	}
	more_argv.push_back(const_cast<char*>("-s"));
	
	mkdir( (origin_path + dotCow ).c_str(), 0777 );
	mkdir( (origin_path + dotCow+ "/filedata").c_str(), 0777 );
	db.open(origin_path + dotCow+ "/history.db");
	db.exec("pragma synchronous = NORMAL");
	
	db.exec("create table if not exists historical_files (path primary key, command, data)");
	db.exec("create table if not exists new_files (path primary key, command)");
	db.exec("create index if not exists historical_renames on historical_files (data,command)");

	origin_fd = ::open(origin_path.c_str(), O_DIRECTORY);
	if (origin_fd == -1)
		throw std::runtime_error("failed to open");
	
	return fuse_main(more_argv.size(), &more_argv.front(), &cow_oper, nullptr);
}

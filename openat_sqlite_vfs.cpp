#include <sqlite3.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>

#include <stdexcept>

extern int origin_fd;

static int replace_open(const char *pathname, int flags, int mode)
{
	if (pathname[0] == '/')
		return open(pathname, flags, mode);
	else
		return openat(origin_fd, pathname, flags, mode);
}

static int replace_access(const char *pathname, int mode)
{
	if (pathname[0] == '/')
		return access(pathname, mode);
	else
		return faccessat(origin_fd, pathname, mode, 0);
}

static int replace_stat(const char *pathname, struct stat *buf)
{
	if (pathname[0] == '/')
		return stat(pathname, buf);
	else
		return fstatat(origin_fd, pathname, buf, 0);
}

static int replace_unlink(const char *pathname)
{
	if (pathname[0] == '/')
		return unlink(pathname);
	else
		return unlinkat(origin_fd, pathname, 0);
}

static int replace_mkdir(const char *pathname, mode_t mode)
{
	if (pathname[0] == '/')
		return mkdir(pathname, mode);
	else
		return mkdirat(origin_fd, pathname, mode);
}

static int replace_rmdir(const char *pathname)
{
	if (pathname[0] == '/')
		return rmdir(pathname);
	else
		return unlinkat(origin_fd, pathname, AT_REMOVEDIR);
}

static int atFullPathname(sqlite3_vfs *, const char *zName, int nOut, char *zOut)
{
	std::strncpy(zOut, zName, nOut);
	return SQLITE_OK;
}


static sqlite3_vfs vfs_openat;

void register_openat_vfs()
{
	sqlite3_vfs *const def = sqlite3_vfs_find(nullptr);
	
	vfs_openat = *def;
	vfs_openat.pNext = nullptr;
	vfs_openat.zName = "openat";
	vfs_openat.xFullPathname = atFullPathname;
	
	vfs_openat.xSetSystemCall(&vfs_openat, "open", (sqlite3_syscall_ptr)replace_open);
	vfs_openat.xSetSystemCall(&vfs_openat, "access", (sqlite3_syscall_ptr)replace_access);
	vfs_openat.xSetSystemCall(&vfs_openat, "stat", (sqlite3_syscall_ptr)replace_stat);
	vfs_openat.xSetSystemCall(&vfs_openat, "unlink", (sqlite3_syscall_ptr)replace_unlink);
	vfs_openat.xSetSystemCall(&vfs_openat, "mkdir", (sqlite3_syscall_ptr)replace_mkdir);
	vfs_openat.xSetSystemCall(&vfs_openat, "rmdir", (sqlite3_syscall_ptr)replace_rmdir);
	
	if (SQLITE_OK != sqlite3_vfs_register(&vfs_openat, 1))
	{
		throw std::runtime_error("Failed to replace vfs");
	}
}

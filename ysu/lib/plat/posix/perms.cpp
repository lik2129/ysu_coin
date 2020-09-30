#include <ysu/lib/utility.hpp>

#include <boost/filesystem.hpp>

#include <sys/stat.h>
#include <sys/types.h>

void ysu::set_umask ()
{
	umask (077);
}

void ysu::set_secure_perm_directory (boost::filesystem::path const & path)
{
	boost::filesystem::permissions (path, boost::filesystem::owner_all);
}

void ysu::set_secure_perm_directory (boost::filesystem::path const & path, boost::system::error_code & ec)
{
	boost::filesystem::permissions (path, boost::filesystem::owner_all, ec);
}

void ysu::set_secure_perm_file (boost::filesystem::path const & path)
{
	boost::filesystem::permissions (path, boost::filesystem::perms::owner_read | boost::filesystem::perms::owner_write);
}

void ysu::set_secure_perm_file (boost::filesystem::path const & path, boost::system::error_code & ec)
{
	boost::filesystem::permissions (path, boost::filesystem::perms::owner_read | boost::filesystem::perms::owner_write, ec);
}

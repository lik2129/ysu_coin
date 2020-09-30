namespace boost
{
namespace filesystem
{
	class path;
}
}

namespace ysu
{
class node_flags;
}
namespace ysu_daemon
{
class daemon
{
public:
	void run (boost::filesystem::path const &, ysu::node_flags const & flags);
};
}

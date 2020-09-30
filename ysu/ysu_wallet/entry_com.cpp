#include <ysu/lib/errors.hpp>
#include <ysu/lib/utility.hpp>
#include <ysu/node/cli.hpp>
#include <ysu/rpc/rpc.hpp>
#include <ysu/secure/utility.hpp>
#include <ysu/secure/working.hpp>

#include <boost/format.hpp>
#include <boost/make_shared.hpp>
#include <boost/program_options.hpp>

int main (int argc, char * const * argv)
{
	ysu::set_umask ();
	try
	{
		boost::program_options::options_description description ("Command line options");
		description.add_options () ("help", "Print out options");
		ysu::add_node_options (description);
		boost::program_options::variables_map vm;
		boost::program_options::store (boost::program_options::command_line_parser (argc, argv).options (description).allow_unregistered ().run (), vm);
		boost::program_options::notify (vm);
		int result (0);

		if (!vm.count ("data_path"))
		{
			std::string error_string;
			if (!ysu::migrate_working_path (error_string))
			{
				throw std::runtime_error (error_string);
			}
		}

		auto ec = ysu::handle_node_options (vm);
		if (ec == ysu::error_cli::unknown_command && vm.count ("help") != 0)
		{
			std::cout << description << std::endl;
		}
		return result;
	}
	catch (std::exception const & e)
	{
		std::cerr << boost::str (boost::format ("Exception while initializing %1%") % e.what ());
	}
	catch (...)
	{
		std::cerr << boost::str (boost::format ("Unknown exception while initializing"));
	}
	return 1;
}

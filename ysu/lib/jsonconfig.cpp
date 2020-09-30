#include <ysu/boost/asio/ip/address_v6.hpp>
#include <ysu/lib/jsonconfig.hpp>

#include <boost/filesystem/convenience.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <cstddef>

ysu::jsonconfig::jsonconfig () :
tree (tree_default)
{
	error = std::make_shared<ysu::error> ();
}

ysu::jsonconfig::jsonconfig (boost::property_tree::ptree & tree_a, std::shared_ptr<ysu::error> error_a) :
ysu::configbase (error_a), tree (tree_a)
{
	if (!error)
	{
		error = std::make_shared<ysu::error> ();
	}
}

/**
 * Reads a json object from the stream 
 * @return ysu::error&, including a descriptive error message if the config file is malformed.
 */
ysu::error & ysu::jsonconfig::read (boost::filesystem::path const & path_a)
{
	std::fstream stream;
	open_or_create (stream, path_a.string ());
	if (!stream.fail ())
	{
		try
		{
			boost::property_tree::read_json (stream, tree);
		}
		catch (std::runtime_error const & ex)
		{
			auto pos (stream.tellg ());
			if (pos != std::streampos (0))
			{
				*error = ex;
			}
		}
		stream.close ();
	}
	return *error;
}

void ysu::jsonconfig::write (boost::filesystem::path const & path_a)
{
	std::fstream stream;
	open_or_create (stream, path_a.string ());
	write (stream);
}

void ysu::jsonconfig::write (std::ostream & stream_a) const
{
	boost::property_tree::write_json (stream_a, tree);
}

void ysu::jsonconfig::read (std::istream & stream_a)
{
	boost::property_tree::read_json (stream_a, tree);
}

/** Open configuration file, create if necessary */
void ysu::jsonconfig::open_or_create (std::fstream & stream_a, std::string const & path_a)
{
	if (!boost::filesystem::exists (path_a))
	{
		// Create temp stream to first create the file
		std::ofstream stream (path_a);

		// Set permissions before opening otherwise Windows only has read permissions
		ysu::set_secure_perm_file (path_a);
	}

	stream_a.open (path_a);
}

/** Takes a filepath, appends '_backup_<timestamp>' to the end (but before any extension) and saves that file in the same directory */
void ysu::jsonconfig::create_backup_file (boost::filesystem::path const & filepath_a)
{
	auto extension = filepath_a.extension ();
	auto filename_without_extension = filepath_a.filename ().replace_extension ("");
	auto orig_filepath = filepath_a;
	auto & backup_path = orig_filepath.remove_filename ();
	auto backup_filename = filename_without_extension;
	backup_filename += "_backup_";
	backup_filename += std::to_string (std::chrono::system_clock::now ().time_since_epoch ().count ());
	backup_filename += extension;
	auto backup_filepath = backup_path / backup_filename;

	boost::filesystem::copy_file (filepath_a, backup_filepath);
}

/** Returns the boost property node managed by this instance */
boost::property_tree::ptree const & ysu::jsonconfig::get_tree ()
{
	return tree;
}

/** Returns true if the property tree node is empty */
bool ysu::jsonconfig::empty () const
{
	return tree.empty ();
}

boost::optional<ysu::jsonconfig> ysu::jsonconfig::get_optional_child (std::string const & key_a)
{
	boost::optional<jsonconfig> child_config;
	auto child = tree.get_child_optional (key_a);
	if (child)
	{
		return jsonconfig (child.get (), error);
	}
	return child_config;
}

ysu::jsonconfig ysu::jsonconfig::get_required_child (std::string const & key_a)
{
	auto child = tree.get_child_optional (key_a);
	if (!child)
	{
		*error = ysu::error_config::missing_value;
		error->set_message ("Missing configuration node: " + key_a);
	}
	return child ? jsonconfig (child.get (), error) : *this;
}

ysu::jsonconfig & ysu::jsonconfig::put_child (std::string const & key_a, ysu::jsonconfig & conf_a)
{
	tree.add_child (key_a, conf_a.get_tree ());
	return *this;
}

ysu::jsonconfig & ysu::jsonconfig::replace_child (std::string const & key_a, ysu::jsonconfig & conf_a)
{
	tree.erase (key_a);
	put_child (key_a, conf_a);
	return *this;
}

/** Returns true if \p key_a is present */
bool ysu::jsonconfig::has_key (std::string const & key_a)
{
	return tree.find (key_a) != tree.not_found ();
}

/** Erase the property of given key */
ysu::jsonconfig & ysu::jsonconfig::erase (std::string const & key_a)
{
	tree.erase (key_a);
	return *this;
}

// boost's lexical cast doesn't handle (u)int8_t
ysu::jsonconfig & ysu::jsonconfig::get_config (bool optional, std::string key, uint8_t & target, uint8_t default_value)
{
	int64_t tmp;
	try
	{
		auto val (tree.get<std::string> (key));
		if (!boost::conversion::try_lexical_convert<int64_t> (val, tmp) || tmp < 0 || tmp > 255)
		{
			conditionally_set_error<uint8_t> (ysu::error_config::invalid_value, optional, key);
		}
		else
		{
			target = static_cast<uint8_t> (tmp);
		}
	}
	catch (boost::property_tree::ptree_bad_path const &)
	{
		if (!optional)
		{
			conditionally_set_error<uint8_t> (ysu::error_config::missing_value, optional, key);
		}
		else
		{
			target = default_value;
		}
	}
	catch (std::runtime_error & ex)
	{
		conditionally_set_error<uint8_t> (ex, optional, key);
	}
	return *this;
}

ysu::jsonconfig & ysu::jsonconfig::get_config (bool optional, std::string key, bool & target, bool default_value)
{
	auto bool_conv = [this, &target, &key, optional](std::string val) {
		if (val == "true")
		{
			target = true;
		}
		else if (val == "false")
		{
			target = false;
		}
		else if (!*error)
		{
			conditionally_set_error<bool> (ysu::error_config::invalid_value, optional, key);
		}
	};
	try
	{
		auto val (tree.get<std::string> (key));
		bool_conv (val);
	}
	catch (boost::property_tree::ptree_bad_path const &)
	{
		if (!optional)
		{
			conditionally_set_error<bool> (ysu::error_config::missing_value, optional, key);
		}
		else
		{
			target = default_value;
		}
	}
	catch (std::runtime_error & ex)
	{
		conditionally_set_error<bool> (ex, optional, key);
	}
	return *this;
}

ysu::jsonconfig & ysu::jsonconfig::get_config (bool optional, std::string key, boost::asio::ip::address_v6 & target, boost::asio::ip::address_v6 const & default_value)
{
	try
	{
		auto address_l (tree.get<std::string> (key));
		boost::system::error_code bec;
		target = boost::asio::ip::make_address_v6 (address_l, bec);
		if (bec)
		{
			conditionally_set_error<boost::asio::ip::address_v6> (ysu::error_config::invalid_value, optional, key);
		}
	}
	catch (boost::property_tree::ptree_bad_path const &)
	{
		if (!optional)
		{
			conditionally_set_error<boost::asio::ip::address_v6> (ysu::error_config::missing_value, optional, key);
		}
		else
		{
			target = default_value;
		}
	}
	return *this;
}

void ysu::jsonconfig::write_json (std::fstream & stream)
{
	boost::property_tree::write_json (stream, tree);
}

#pragma once

#include <ysu/lib/work.hpp>
#include <ysu/node/openclconfig.hpp>
#include <ysu/node/xorshift.hpp>

#include <boost/optional.hpp>

#include <atomic>
#include <mutex>
#include <vector>

#ifdef __APPLE__
#define CL_SILENCE_DEPRECATION
#include <OpenCL/opencl.h>
#else
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#include <CL/cl.h>
#endif

namespace ysu
{
extern bool opencl_loaded;
class logger_mt;
class opencl_platform
{
public:
	cl_platform_id platform;
	std::vector<cl_device_id> devices;
};
class opencl_environment
{
public:
	opencl_environment (bool &);
	void dump (std::ostream & stream);
	std::vector<ysu::opencl_platform> platforms;
};
class root;
class work_pool;
class opencl_work
{
public:
	opencl_work (bool &, ysu::opencl_config const &, ysu::opencl_environment &, ysu::logger_mt &);
	~opencl_work ();
	boost::optional<uint64_t> generate_work (ysu::work_version const, ysu::root const &, uint64_t const);
	boost::optional<uint64_t> generate_work (ysu::work_version const, ysu::root const &, uint64_t const, std::atomic<int> &);
	static std::unique_ptr<opencl_work> create (bool, ysu::opencl_config const &, ysu::logger_mt &);
	ysu::opencl_config const & config;
	std::mutex mutex;
	cl_context context;
	cl_mem attempt_buffer;
	cl_mem result_buffer;
	cl_mem item_buffer;
	cl_mem difficulty_buffer;
	cl_program program;
	cl_kernel kernel;
	cl_command_queue queue;
	ysu::xorshift1024star rand;
	ysu::logger_mt & logger;
};
}

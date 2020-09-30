#include <ysu/lib/threading.hpp>

#include <pthread.h>
#include <pthread_np.h>

void ysu::thread_role::set_os_name (std::string const & thread_name)
{
	pthread_set_name_np (pthread_self (), thread_name.c_str ());
}

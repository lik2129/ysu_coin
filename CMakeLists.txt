cmake_minimum_required (VERSION 3.4)

if (CMAKE_VERSION VERSION_GREATER 3.12 OR CMAKE_VERSION VERSION_EQUAL 3.12)
	#find_package uses <PACKAGENAME>_ROOT variables
	cmake_policy(SET CMP0074 NEW)
endif()
if (CMAKE_VERSION VERSION_GREATER 3.13 OR CMAKE_VERSION VERSION_EQUAL 3.13)
	#option honors normal variables
	cmake_policy(SET CMP0077 NEW)
endif()

if (CMAKE_VERSION VERSION_LESS 3.13)
	# compatibility for boost import targets use bundled 3.13 FindBoost.cmake
	list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/legacyModules")
endif ()

# compatibility for osx sierra and on
# needs to be set before project
set (CMAKE_OSX_DEPLOYMENT_TARGET 10.14 CACHE STRING "")

project (ysu-node)

# Get the latest abbreviated commit hash of the working branch
execute_process(
  COMMAND git log -1 --format=%h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_COMMIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

option (CI_BUILD false)
set (CI_TEST 0 CACHE STRING "")

if(MSVC)
  add_definitions(/MP)
endif()

set (CPACK_PACKAGE_VERSION_MAJOR "22")
set (CPACK_PACKAGE_VERSION_MINOR "0")
set (CPACK_PACKAGE_VERSION_PATCH "0")
set (CPACK_PACKAGE_VERSION_PRE_RELEASE "0")
set (CPACK_PACKAGE_VENDOR "Ysu Currency")

if (CI_BUILD)
	set (TAG_VERSION_STRING "$ENV{TRAVIS_TAG}")
else()
	set (TAG_VERSION_STRING "V${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}")
endif()

if (APPLE)
	set (CMAKE_INSTALL_RPATH "@executable_path/../Frameworks;@executable_path/../boost/lib")
else()
	set (CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
endif()

# Create all libraries and executables in the root binary dir
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set (YSU_GUI OFF CACHE BOOL "")
set (YSU_TEST OFF CACHE BOOL "")
set (YSU_SECURE_RPC OFF CACHE BOOL "")
set (YSU_POW_SERVER OFF CACHE BOOL "")
set (YSU_WARN_TO_ERR OFF CACHE BOOL "")
set (YSU_TIMED_LOCKS 0 CACHE STRING "")
set (YSU_TIMED_LOCKS_IGNORE_BLOCKED OFF CACHE BOOL "")
set (YSU_FUZZER_TEST OFF CACHE BOOL "")
set (YSU_ASIO_HANDLER_TRACKING 0 CACHE STRING "")
set (YSU_ROCKSDB_TOOLS OFF CACHE BOOL "")

option (YSU_STACKTRACE_BACKTRACE "Use BOOST_STACKTRACE_USE_BACKTRACE in stacktraces, for POSIX" OFF)
if (YSU_STACKTRACE_BACKTRACE)
	add_definitions(-DYSU_STACKTRACE_BACKTRACE=1)
	if (BACKTRACE_INCLUDE)
		add_definitions(-DBOOST_STACKTRACE_BACKTRACE_INCLUDE_FILE=${BACKTRACE_INCLUDE})
	endif()
endif ()

if (${YSU_TIMED_LOCKS} GREATER 0)
	add_definitions (-DYSU_TIMED_LOCKS=${YSU_TIMED_LOCKS})
	if (YSU_TIMED_LOCKS_IGNORE_BLOCKED)
		add_definitions(-DYSU_TIMED_LOCKS_IGNORE_BLOCKED)
	endif ()
endif ()

if (${YSU_ASIO_HANDLER_TRACKING} GREATER 0)
	add_definitions (-DYSU_ASIO_HANDLER_TRACKING=${YSU_ASIO_HANDLER_TRACKING} -DBOOST_ASIO_ENABLE_HANDLER_TRACKING)
endif ()

option(YSU_ASAN_INT "Enable ASan+UBSan+Integer overflow" OFF)
option(YSU_ASAN "Enable ASan+UBSan" OFF)
option(YSU_TSAN "Enable TSan" OFF)
option(YSU_SIMD_OPTIMIZATIONS "Enable CPU-specific SIMD optimizations (SSE/AVX or NEON, e.g.)" OFF)
option(ENABLE_AES "Enable AES optimizations (enabled by default with YSU_SIMD_OPTIMIZATIONS, set OFF to disable" ON)
option(ENABLE_AVX2 "Enable AVX2 optimizations" OFF)

SET (ACTIVE_NETWORK ysu_live_network CACHE STRING "Selects which network parameters are used")
set_property (CACHE ACTIVE_NETWORK PROPERTY STRINGS ysu_dev_network ysu_beta_network ysu_live_network ysu_test_network)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(USING_ASAN (YSU_ASAN OR RAIBLOCKS_ASAN))
set(USING_ASAN_INT (YSU_ASAN_INT OR RAIBLOCKS_ASAN_INT))
set(USING_TSAN (YSU_TSAN OR RAIBLOCKS_TSAN))

find_package(Threads)

if (WIN32)
	find_library (PSAPI Psapi)
	add_definitions(-D_WIN32_WINNT=0x0600
					-DWINVER=0x0600
					-DWIN32_LEAN_AND_MEAN
					-DMINIUPNP_STATICLIB
					-D_CRT_SECURE_NO_WARNINGS
					/EHsc)

	if (${USING_TSAN} OR ${USING_ASAN} OR ${USING_ASAN_INT})
		message (WARNING "Cannot use TSAN or ASAN on Windows, sanitizers ignored")
	endif()

else ()
	if (YSU_WARN_TO_ERR)
		add_compile_options(-Werror)
	endif ()

	if ((${USING_TSAN} AND ${USING_ASAN}) OR
	    (${USING_TSAN} AND ${USING_ASAN_INT}))
		message (WARNING "Cannot use TSAN/ASAN together, defaulting to ASAN")
	endif()

	if (${USING_ASAN} OR ${USING_ASAN_INT})
		if (${USING_ASAN_INT})
			add_compile_options(-fsanitize=address,undefined,integer)
		else ()	
			add_compile_options(-fsanitize=address,undefined)
		endif()
		add_definitions(-DED25519_NO_INLINE_ASM)
	elseif (${USING_TSAN})
		add_compile_options(-fsanitize=thread)
		if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
			add_compile_options("-fsanitize-blacklist=${PROJECT_SOURCE_DIR}/tsan_clang_blacklist")
		endif()
		add_definitions(-DED25519_NO_INLINE_ASM)
	endif()

	if (YSU_FUZZER_TEST)
		add_compile_options (-fsanitize=fuzzer-no-link -fno-omit-frame-pointer)
		add_definitions (-DYSU_FUZZER_TEST)
	endif ()

	if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86(_64)?)$")
		if (YSU_SIMD_OPTIMIZATIONS OR RAIBLOCKS_SIMD_OPTIMIZATIONS OR ENABLE_AVX2)
			add_compile_options(-msse4)
			if (ENABLE_AES)
				add_compile_options(-maes)
			else ()
				add_definitions(-DCRYPTOPP_DISABLE_AESNI)
			endif ()
		elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
			add_compile_options(-msse2)
			add_definitions(-DCRYPTOPP_DISABLE_SSSE3
							-DCRYPTOPP_DISABLE_AESNI)
		endif()

	elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
		if (YSU_SIMD_OPTIMIZATIONS OR RAIBLOCKS_SIMD_OPTIMIZATIONS)
			add_compile_options(-march=armv8-a+crc+crypto)
		else ()
			add_compile_options(-march=armv8-a)
			add_definitions(-DCRYPTOPP_DISABLE_ASM)
		endif ()
	endif()
endif ()

if (ENABLE_AVX2)
	if (WIN32)
		add_definitions(/arch:AVX2)
	elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86(_64)?)$")
		add_compile_options(-mavx2 -mbmi -mbmi2 -maes)
	endif()
	if (PERMUTE_WITH_GATHER)
		add_definitions(-DPERMUTE_WITH_GATHER)
	elseif (PERMUTE_WITH_SHUFFLES)
		add_definitions(-DPERMUTE_WITH_SHUFFLES)
	endif()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
#set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_CXX_EXTENSIONS OFF)

if (WIN32)
	set (PLATFORM_LINK_FLAGS "") 
	add_definitions(/bigobj)
else ()
	if (APPLE)
		set (PLATFORM_LINK_FLAGS "-framework Foundation -framework OpenCL")
	else ()
		set (PLATFORM_LINK_FLAGS "-static-libgcc -static-libstdc++")
	endif ()

	if (${USING_ASAN_INT})
		set (PLATFORM_LINK_FLAGS "${PLATFORM_LINK_FLAGS} -fsanitize=address,undefined,integer")
	elseif (${USING_ASAN})
		set (PLATFORM_LINK_FLAGS "${PLATFORM_LINK_FLAGS} -fsanitize=address,undefined")
	elseif (${USING_TSAN})
		set (PLATFORM_LINK_FLAGS "${PLATFORM_LINK_FLAGS} -fsanitize=thread")
		if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
			set (PLATFORM_LINK_FLAGS "${PLATFORM_LINK_FLAGS} -fsanitize-blacklist=${PROJECT_SOURCE_DIR}/tsan_clang_blacklist")
		endif()
	endif()
	if (YSU_FUZZER_TEST)
		set (PLATFORM_LINK_FLAGS "${PLATFORM_LINK_FLAGS} -fsanitize=fuzzer-no-link")
	endif ()
endif ()

SET( CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${PLATFORM_LINK_FLAGS}" )

if (YSU_SECURE_RPC OR RAIBLOCKS_SECURE_RPC)
	find_package (OpenSSL 1.0 REQUIRED)
	include_directories(${OPENSSL_INCLUDE_DIR})
	add_definitions (-DYSU_SECURE_RPC)
	message("OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
	message("OpenSSL libraries: ${OPENSSL_LIBRARIES}")
	message("OpenSSL lib: ${OPENSSL_SSL_LIBRARY}")
	message("Crypto lib: ${OPENSSL_CRYPTO_LIBRARY}")
else ()
	set (OPENSSL_LIBRARIES "")
endif ()

include_directories (${CMAKE_SOURCE_DIR})

if (WIN32 AND YSU_TEST AND YSU_SHARED_BOOST)
	message (SEND_ERROR 
		" Linking errors occur if YSU_SHARED_BOOST is used with tests on Windows"
		" Disable YSU_SHARED_BOOST or YSU_TEST on Windows")
	set(YSU_SHARED_BOOST)
endif()

set(YSU_SHARED_BOOST OFF CACHE BOOL "Build Ysu with shared boost")

if (YSU_SHARED_BOOST)
	SET(Boost_USE_STATIC_LIBS OFF)
	SET(Boost_USE_STATIC_RUNTIME OFF)
	SET(Boost_NO_BOOST_CMAKE ON)
	add_definitions(-DBOOST_ALL_DYN_LINK -DBoost_ALL_NO_LIB)
else()
	set(Boost_USE_STATIC_LIBS ON)
endif()
set(Boost_USE_MULTITHREADED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/Modules")

find_package (Boost 1.69.0 REQUIRED COMPONENTS coroutine context filesystem log log_setup thread program_options system)

# RocksDB
include_directories(rocksdb/include)
set(USE_RTTI ON CACHE BOOL "")
set(WITH_GFLAGS OFF CACHE BOOL "")
set(WITH_TESTS OFF CACHE BOOL "")
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "")
set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "")
set(WITH_CORE_TOOLS ${YSU_ROCKSDB_TOOLS} CACHE BOOL "" FORCE)
set(WITH_TOOLS ${YSU_ROCKSDB_TOOLS} CACHE BOOL "" FORCE)
if (ENABLE_AVX2)
	set(PORTABLE OFF CACHE BOOL "" FORCE)
else()
	set(PORTABLE ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(rocksdb EXCLUDE_FROM_ALL)

# There is a compile bug with boost 1.69 interprocess headers on Mac
if (APPLE AND Boost_VERSION EQUAL 106900)
	set (BOOST_PROCESS_SUPPORTED 0)
else ()
	set (BOOST_PROCESS_SUPPORTED 1)
endif ()

include_directories(cpptoml/include)
add_subdirectory(crypto/ed25519-donna)

add_subdirectory(ysu/ipc_flatbuffers_lib)
add_subdirectory(ysu/ipc_flatbuffers_test)

set (UPNPC_BUILD_SHARED OFF CACHE BOOL "")
add_subdirectory (miniupnp/miniupnpc EXCLUDE_FROM_ALL)
# FIXME: This fixes miniupnpc include directories without modifying miniupnpc's
# CMakeLists.txt but should be set there
set_target_properties(libminiupnpc-static PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
	"${CMAKE_SOURCE_DIR}/miniupnp/miniupnpc")

set (BUILD_SHARED OFF CACHE BOOL "")
set (BUILD_TESTING OFF CACHE BOOL "")
set (USE_INTERMEDIATE_OBJECTS_TARGET OFF CACHE BOOL "")
set (CRYPTOPP_EXTRA "")
if (WIN32)
	set (CRYPTOPP_EXTRA crypto/cryptopp/x64dll.asm
					crypto/cryptopp/x64masm.asm)
	enable_language(ASM)
	enable_language(ASM_MASM)
	# similar to SSE2 settings
	add_definitions(-DCRYPTOPP_DISABLE_SSSE3
					-DCRYPTOPP_DISABLE_AESNI)
elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" AND (YSU_SIMD_OPTIMIZATIONS OR RAIBLOCKS_SIMD_OPTIMIZATIONS))
	set (CRYPTOPP_EXTRA crypto/cryptopp/crc_simd.cpp
					crypto/cryptopp/gcm_simd.cpp
					crypto/cryptopp/gf2n_simd.cpp
					crypto/cryptopp/neon_simd.cpp)
	add_definitions(-DCRYPTOPP_NO_CPU_FEATURE_PROBES)
endif ()
# Some Clang cannot handle mixed asm with positional arguments, where the
# body is Intel style with no prefix and the templates are AT&T style.
# See: crypto/cryptopp/config.h
# Also see https://bugs.llvm.org/show_bug.cgi?id=39895
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	add_definitions(-DCRYPTOPP_DISABLE_MIXED_ASM -DCRYPTOPP_DISABLE_ASM)
	message("CryptoPP with disabled ASM for ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif ()
add_definitions(-DCRYPTOPP_DISABLE_SHANI)
# Fix failing builds after commit https://github.com/weidai11/cryptopp/commit/df9fa62205f2d341e2b1b26595a3a1b6377c60c5
add_definitions(-DCRYPTOPP_DISABLE_CLMUL)

set (CRYPTOPP_LIBRARY cryptopp)
add_library (cryptopp
	crypto/cryptopp/algparam.cpp
	crypto/cryptopp/asn.cpp
	crypto/cryptopp/basecode.cpp
	crypto/cryptopp/cpu.cpp
	crypto/cryptopp/cryptlib.cpp
	crypto/cryptopp/default.cpp
	crypto/cryptopp/des.cpp
	crypto/cryptopp/dessp.cpp
	crypto/cryptopp/dll.cpp
	crypto/cryptopp/ec2n.cpp
	crypto/cryptopp/ecp.cpp
	crypto/cryptopp/filters.cpp
	crypto/cryptopp/fips140.cpp
	crypto/cryptopp/gcm.cpp
	crypto/cryptopp/gf2n.cpp
	crypto/cryptopp/gfpcrypt.cpp
	crypto/cryptopp/hex.cpp
	crypto/cryptopp/hmac.cpp
	crypto/cryptopp/hrtimer.cpp
	crypto/cryptopp/integer.cpp
	crypto/cryptopp/iterhash.cpp
	crypto/cryptopp/misc.cpp
	crypto/cryptopp/modes.cpp
	crypto/cryptopp/mqueue.cpp
	crypto/cryptopp/nbtheory.cpp
	crypto/cryptopp/oaep.cpp
	crypto/cryptopp/osrng.cpp
	crypto/cryptopp/pubkey.cpp
	crypto/cryptopp/queue.cpp
	crypto/cryptopp/randpool.cpp
	crypto/cryptopp/rdtables.cpp
	crypto/cryptopp/rijndael.cpp
	crypto/cryptopp/rijndael_simd.cpp
	crypto/cryptopp/rng.cpp
	crypto/cryptopp/sha.cpp
	crypto/cryptopp/sha_simd.cpp
	crypto/cryptopp/simple.cpp
	crypto/cryptopp/sse_simd.cpp
	${CRYPTOPP_EXTRA})

if (WIN32 OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86(_64)?)$")
	set (ARGON_CORE crypto/phc-winner-argon2/src/opt.c)
else ()
	set (ARGON_CORE crypto/phc-winner-argon2/src/ref.c)
endif ()

add_library (argon2
	crypto/phc-winner-argon2/src/argon2.c
	crypto/phc-winner-argon2/include/argon2.h
	crypto/phc-winner-argon2/src/core.c
	crypto/phc-winner-argon2/src/thread.c
	crypto/phc-winner-argon2/src/encoding.c
	${ARGON_CORE})

target_include_directories(argon2 PUBLIC crypto/phc-winner-argon2/include)
target_include_directories(argon2 PUBLIC crypto/phc-winner-argon2/src)
target_include_directories(argon2 PUBLIC crypto/blake2)

add_library (lmdb
	lmdb/libraries/liblmdb/lmdb.h
	lmdb/libraries/liblmdb/mdb.c
	lmdb/libraries/liblmdb/midl.c)

if (WIN32)
	target_link_libraries(lmdb ntdll)
endif()

if (WIN32)
	set (BLAKE2_IMPLEMENTATION "crypto/blake2/blake2b.c")
else ()
	IF (CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86(_64)?)$")
		set (BLAKE2_IMPLEMENTATION "crypto/blake2/blake2b.c")
	else()
		set (BLAKE2_IMPLEMENTATION "crypto/blake2/blake2b-ref.c")
	endif()
endif ()

add_library (blake2
	crypto/blake2/blake2-config.h
	crypto/blake2/blake2-impl.h
	crypto/blake2/blake2.h
	${BLAKE2_IMPLEMENTATION})

target_compile_definitions(blake2 PRIVATE -D__SSE2__)

if (YSU_POW_SERVER)
	add_subdirectory(ysu-pow-server)
endif ()
add_subdirectory(ysu/crypto_lib)
add_subdirectory(ysu/secure)
add_subdirectory(ysu/lib)
add_subdirectory(ysu/node)
add_subdirectory(ysu/ysu_node)
add_subdirectory(ysu/rpc)
add_subdirectory(ysu/ysu_rpc)

if (YSU_FUZZER_TEST)
	if (NOT WIN32)
		add_subdirectory (ysu/fuzzer_test)
	else ()
		message (WARNING "Fuzzing is not supported on Windows")
	endif ()
endif ()

if (YSU_TEST OR RAIBLOCKS_TEST)
	if(WIN32)
		if(MSVC_VERSION)
			if(MSVC_VERSION GREATER_EQUAL 1910)
				add_definitions(-DGTEST_LANG_CXX11=1)
				add_definitions(-DGTEST_HAS_TR1_TUPLE=0)
			endif()
		endif()
		set (gtest_force_shared_crt ON)
	else ()
		set (gtest_force_shared_crt OFF)
	endif()

	add_subdirectory(ysu/load_test)

	add_subdirectory (gtest/googletest)
	# FIXME: This fixes gtest include directories without modifying gtest's
	# CMakeLists.txt. Ideally we should use GTest::GTest and GTest::Main as
	# dependencies but it requires building gtest differently
	set_target_properties(gtest PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
		"${CMAKE_SOURCE_DIR}/gtest/googletest/include")

	add_subdirectory(ysu/test_common)
	add_subdirectory(ysu/core_test)
	add_subdirectory(ysu/rpc_test)
	add_subdirectory(ysu/slow_test)
endif ()
if (YSU_GUI OR RAIBLOCKS_GUI)
	install (FILES ${PROJECT_BINARY_DIR}/config-node.toml.sample DESTINATION .)
	install (FILES ${PROJECT_BINARY_DIR}/config-rpc.toml.sample DESTINATION .)
	if (WIN32)
		set (PLATFORM_QT_PACKAGES WinExtras)
	else ()
		set (PLATFORM_QT_PACKAGES)
	endif ()

	find_package (Qt5 COMPONENTS Core Gui Widgets Test ${PLATFORM_QT_PACKAGES})

	add_library (qt
		ysu/qt/qt.cpp
		ysu/qt/qt.hpp)

	target_link_libraries(qt
		node secure ysu_lib libminiupnpc-static Qt5::Gui Qt5::Widgets)

	target_compile_definitions(qt
		PRIVATE
		-DTAG_VERSION_STRING=${TAG_VERSION_STRING}
		-DGIT_COMMIT_HASH=${GIT_COMMIT_HASH})

	if (WIN32)
		set (PLATFORM_GUI_TARGET_PROPERTIES WIN32)
	else ()
		set (PLATFORM_GUI_TARGET_PROPERTIES "")
	endif ()

	if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
		set (PLATFORM_WALLET_SOURCE ysu/ysu_wallet/plat/default/icon.cpp)
	elseif (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
		set (PLATFORM_WALLET_SOURCE ysu/ysu_wallet/plat/windows/icon.cpp Ysu.rc)
	elseif (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
		set (PLATFORM_WALLET_SOURCE ysu/ysu_wallet/plat/default/icon.cpp)
	elseif (${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
		set (PLATFORM_WALLET_SOURCE ysu/ysu_wallet/plat/default/icon.cpp)
	else ()
		error ("Unknown platform: ${CMAKE_SYSTEM_NAME}")
	endif ()

	qt5_add_resources(RES resources.qrc)

	add_executable (ysu_wallet ${PLATFORM_GUI_TARGET_PROPERTIES}
		${PLATFORM_WALLET_SOURCE}
		ysu/ysu_wallet/entry.cpp
		ysu/ysu_wallet/icon.hpp
		${RES})

	target_link_libraries (ysu_wallet
		rpc
		node
		qt)

	target_compile_definitions(ysu_wallet
		PRIVATE
			-DBOOST_PROCESS_SUPPORTED=${BOOST_PROCESS_SUPPORTED})

	if (WIN32)
		target_link_libraries (ysu_wallet Qt5::WinExtras)
		# ysu_wallet.com executable for Windows console
		add_executable(ysu_wallet_com
			ysu/ysu_wallet/entry_com.cpp)
		target_link_libraries (ysu_wallet_com
			node)
		set_target_properties (ysu_wallet_com PROPERTIES COMPILE_FLAGS "-DBOOST_ASIO_HAS_STD_ARRAY=1" OUTPUT_NAME "ysu_wallet" SUFFIX ".com")
	endif()

	add_executable (qt_system
		ysu/qt_system/entry.cpp)

	target_link_libraries (qt_system qt node Qt5::Gui Qt5::Widgets)

	set_target_properties (qt ysu_wallet qt_system PROPERTIES COMPILE_FLAGS "-DQT_NO_KEYWORDS -DBOOST_ASIO_HAS_STD_ARRAY=1")

	if (YSU_TEST OR RAIBLOCKS_TEST)
		add_executable (qt_test
			ysu/qt_test/entry.cpp
			ysu/qt_test/qt.cpp)

		target_link_libraries(qt_test test_common gtest gtest_main qt Qt5::Test)

		set_target_properties (qt_test PROPERTIES COMPILE_FLAGS "-DQT_NO_KEYWORDS -DBOOST_ASIO_HAS_STD_ARRAY=1")
	endif ()

	if (APPLE)
		set (CPACK_GENERATOR "DragNDrop")
		CONFIGURE_FILE(${CMAKE_SOURCE_DIR}/Info.plist.in ${CMAKE_SOURCE_DIR}/Info.plist @ONLY)
		install (TARGETS ysu_wallet DESTINATION Ysu.app/Contents/MacOS)
		install (TARGETS ysu_node DESTINATION Ysu.app/Contents/MacOS)
		install (TARGETS ysu_rpc DESTINATION Ysu.app/Contents/MacOS)
		install (FILES Info.plist DESTINATION Ysu.app/Contents)
		install (FILES qt.conf DESTINATION Ysu.app/Contents/Resources)
		install (DIRECTORY ${Qt5_DIR}/../../QtCore.framework DESTINATION Ysu.app/Contents/Frameworks)
		install (DIRECTORY ${Qt5_DIR}/../../QtDBus.framework DESTINATION Ysu.app/Contents/Frameworks)
		install (DIRECTORY ${Qt5_DIR}/../../QtGui.framework DESTINATION Ysu.app/Contents/Frameworks)
		install (DIRECTORY ${Qt5_DIR}/../../QtPrintSupport.framework DESTINATION Ysu.app/Contents/Frameworks)
		install (DIRECTORY ${Qt5_DIR}/../../QtTest.framework DESTINATION Ysu.app/Contents/Frameworks)
		install (DIRECTORY ${Qt5_DIR}/../../QtWidgets.framework DESTINATION Ysu.app/Contents/Frameworks)
		install (FILES "${Qt5_DIR}/../../../plugins/platforms/libqcocoa.dylib" DESTINATION Ysu.app/Contents/PlugIns/platforms)
		if (YSU_SHARED_BOOST)
			foreach(boost_lib IN LISTS Boost_LIBRARIES)
				string(REGEX MATCH "(.+/.*boost_[^-]+)" boost_lib_name ${boost_lib})
				set (boost_dll "${CMAKE_MATCH_1}")
				if (${boost_dll} MATCHES "boost")
					install (FILES ${boost_dll} DESTINATION Ysu.app/Contents/boost/lib)
				endif()
			endforeach(boost_lib)
		endif()	
		if (YSU_POW_SERVER)
			install (TARGETS ysu_pow_server DESTINATION Ysu.app/Contents/MacOS)
			install (DIRECTORY ${PROJECT_SOURCE_DIR}/ysu-pow-server/public DESTINATION Ysu.app/Contents/MacOS)
		endif()
		install (FILES Ysu.icns DESTINATION Ysu.app/Contents/Resources)
	elseif (WIN32)
		if ("${CMAKE_SIZEOF_VOID_P}" EQUAL "8")
			set (WIN_REDIST vc_redist.x64.exe)
		else ()
			set (WIN_REDIST vc_redist.x86.exe)
		endif ()
		set (CPACK_NSIS_EXTRA_INSTALL_COMMANDS "ExecWait '\\\"$INSTDIR\\\\${WIN_REDIST}\\\" /quiet /norestart'
												WriteRegDWORD HKLM 'SYSTEM\\\\CurrentControlSet\\\\Services\\\\EventLog\\\\Ysu\\\\Ysu' 'TypesSupported' '0x7'
												WriteRegStr HKLM 'SYSTEM\\\\CurrentControlSet\\\\Services\\\\EventLog\\\\Ysu\\\\Ysu' 'EventMessageFile' '$INSTDIR\\\\ysu_wallet.exe'")
		set (CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "DeleteRegKey HKLM 'SYSTEM\\\\CurrentControlSet\\\\Services\\\\EventLog\\\\Ysu'")
		set (CPACK_PACKAGE_INSTALL_DIRECTORY "ysucurrency")
		set (CPACK_NSIS_MUI_ICON ${CMAKE_SOURCE_DIR}/Ysu.ico)
		set (CPACK_NSIS_DISPLAY_NAME "Ysu")
		set (CPACK_PACKAGE_NAME "ysu-node")
		set (CPACK_NSIS_PACKAGE_NAME "Ysu")
		set (CPACK_NSIS_URL_INFO_ABOUT "https://ysu.org")
		set (CPACK_NSIS_CONTACT "info@ysu.org")
		set (CPACK_NSIS_MENU_LINKS "ysu_wallet.exe" "Ysu Wallet" "https://ysu.org" "Ysu website")
		set (CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
		get_target_property (Qt5WindowsPlugin Qt5::QWindowsIntegrationPlugin LOCATION)
		get_filename_component (Qt5_bin_DIR ${Qt5_DIR}/../../../bin ABSOLUTE)
		install (TARGETS ysu_wallet DESTINATION .)
		install (TARGETS ysu_wallet_com DESTINATION .)
		if (YSU_SHARED_BOOST)
			foreach(boost_lib IN LISTS Boost_LIBRARIES)
				if (${CMAKE_BUILD_TYPE} MATCHES "Rel")
					string(REGEX MATCH "(.+/.*boost_[^-]+-.+-mt-x64.+\)(.lib|a)" boost_lib_name ${boost_lib})
					set (boost_dll "${CMAKE_MATCH_1}.dll")
					if (${boost_dll} MATCHES "boost")
						install (FILES ${boost_dll} DESTINATION .)
					endif()
				else ()
					string(REGEX MATCH "(.+/.*boost_[^-]+-.+-mt-.+-x64.+\)(.lib|a)" boost_lib_name ${boost_lib})
					set (boost_dll "${CMAKE_MATCH_1}.dll")
					if (${boost_dll} MATCHES "boost")
						install (FILES ${boost_dll} DESTINATION .)
					endif()
				endif()
			endforeach(boost_lib)
		endif()
		if (YSU_POW_SERVER)
			install (TARGETS ysu_pow_server DESTINATION .)
			install (DIRECTORY ${PROJECT_SOURCE_DIR}/ysu-pow-server/public DESTINATION .)
		endif ()
		install (FILES ${CMAKE_CURRENT_BINARY_DIR}/${WIN_REDIST} DESTINATION .)
		install (FILES ${Qt5_bin_DIR}/libGLESv2.dll DESTINATION .)
		install (FILES ${Qt5_bin_DIR}/Qt5Core.dll DESTINATION .)
		install (FILES ${Qt5_bin_DIR}/Qt5Gui.dll DESTINATION .)
		install (FILES ${Qt5_bin_DIR}/Qt5Widgets.dll DESTINATION .)
		install (FILES ${Qt5_bin_DIR}/Qt5WinExtras.dll DESTINATION .)
		install (FILES ${Qt5WindowsPlugin} DESTINATION platforms)
	else ()
		set(CPACK_GENERATOR "TBZ2;DEB")
		set(CPACK_DEBIAN_PACKAGE_DEPENDS qt5-default)
		set(CPACK_DEBIAN_PACKAGE_MAINTAINER "russel@ysu.org")
		install(TARGETS ysu_wallet
				RUNTIME DESTINATION ./bin
		)
		if (YSU_SHARED_BOOST)
			foreach(boost_lib IN LISTS Boost_LIBRARIES)
				string(REGEX MATCH "(.+/.*boost_[^-]+)" boost_lib_name ${boost_lib})
				set (boost_dll "${CMAKE_MATCH_1}.${Boost_VERSION_STRING}")
				if (${boost_dll} MATCHES "boost")
					install (FILES ${boost_dll} DESTINATION ./lib)
				endif()
			endforeach(boost_lib)
		endif()
		if (YSU_POW_SERVER)
			install (TARGETS ysu_pow_server DESTINATION ./bin)
			install (DIRECTORY ${PROJECT_SOURCE_DIR}/ysu-pow-server/public DESTINATION ./bin)
		endif ()
	endif ()
endif ()

set (CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
include (CPack)

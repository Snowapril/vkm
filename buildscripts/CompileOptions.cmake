#
# Platform and architecture setup
#

# Set warnings as errors flag
option(VKM_WARNINGS_AS_ERRORS "Treat all warnings as errors" ON)
if(VKM_WARNINGS_AS_ERRORS)
	if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
		set(WARN_AS_ERROR_FLAGS	"/WX")
	else()
		set(WARN_AS_ERROR_FLAGS "-Werror")
	endif()
endif()

# Get upper case system name
string(TOUPPER ${CMAKE_SYSTEM_NAME} SYSTEM_NAME_UPPER)

# Determine architecture (32/64 bit)
set(X64 OFF)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(X64 ON)
endif()

#
# Project options
#

set(DEFAULT_PROJECT_OPTIONS
	# Not available before CMake 3.8.2; see below for manual command line argument addition
	CXX_STANDARD		      20
	CXX_STANDARD_REQUIRED     20
	LINKER_LANGUAGE           "CXX"
	POSITION_INDEPENDENT_CODE ON
)

#
# Include directories
#

set(DEFAULT_INCLUDE_DIRECTORIES)

#
# Libraries
#

set(DEFAULT_LIBRARIES)

#
# Compile definitions
#

set(DEFAULT_COMPILE_DEFINITIONS
	SYSTEM_${SYSTEM_NAME_UPPER}
)

# MSVC compiler options
if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
	set(DEFAULT_COMPILE_DEFINITIONS ${DEFAULT_COMPILE_DEFINITIONS}
		_SCL_SECURE_NO_WARNINGS  # Calling any one of the potentially unsafe methods in the Standard C++ Library
		_CRT_SECURE_NO_WARNINGS  # Calling any one of the potentially unsafe methods in the CRT Library
	)
endif ()

#
# Compile options
#

set(DEFAULT_COMPILE_OPTIONS)

# MSVC compiler options
if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
	# remove default warning level from CMAKE_CXX_FLAGS
	string (REGEX REPLACE "/W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# MSVC compiler options
if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
	set(DEFAULT_COMPILE_OPTIONS ${DEFAULT_COMPILE_OPTIONS}
		/MP           # -> build with multiple processes
		/W4           # -> warning level 3
		${WARN_AS_ERROR_FLAGS}

		/wd4819       # -> disable warning: The file contains a character that cannot be represented in the current code page (949) (caused by pybind11)
		/wd4201       # -> disable warning: nonstandard extension used: nameless struct/union
		/wd4189       # -> disable warning: nonstandard extension used: nameless struct/union
		#$<$<CONFIG:Debug>:
		#/RTCc        # -> value is assigned to a smaller data type and results in a data loss
		#>

		/utf-8 		  # -> spdlog

		$<$<CONFIG:Release>:
		/Gw           # -> whole program global optimization
		/GS-          # -> buffer security check: no
		/GL           # -> whole program optimization: enable link-time code generation (disables Zi)
		/GF           # -> enable string pooling
		>

		# No manual c++11 enable for MSVC as all supported MSVC versions for cmake-init have C++11 implicitly enabled (MSVC >=2013)
	)
endif ()

# GCC and Clang compiler options
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	set(DEFAULT_COMPILE_OPTIONS ${DEFAULT_COMPILE_OPTIONS}
		-Wall
		-Wno-missing-braces
		-Wno-register			# -> disable warning: ISO c++1z does not allow 'register' storage class specifier [-wregister] (caused by pybind11)
        -Wno-error=register		# -> disable warning: ISO c++1z does not allow 'register' storage class specifier [-wregister] (caused by pybind11)
		-Wunused-private-field	# -> disable warning: '...' is a private field of '...'
		${WARN_AS_ERROR_FLAGS}
		-std=c++2a
	)
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
	set(DEFAULT_COMPILE_OPTIONS ${DEFAULT_COMPILE_OPTIONS}
		-Wall
		-Wno-missing-braces
		-Wno-register			# -> disable warning: ISO c++1z does not allow 'register' storage class specifier [-wregister] (caused by pybind11)
        -Wno-error=register		# -> disable warning: ISO c++1z does not allow 'register' storage class specifier [-wregister] (caused by pybind11)
		-Wno-unused-private-field	# -> disable warning: '...' is a private field of '...'
		${WARN_AS_ERROR_FLAGS}
		-std=c++2a
	)
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
	set(DEFAULT_COMPILE_OPTIONS ${DEFAULT_COMPILE_OPTIONS}
		-Wno-int-in-bool-context
	)
endif ()

# Prevent "no matching function for call to 'operator delete'" error
# https://github.com/pybind/pybind11/issues/1604
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	set(DEFAULT_COMPILE_OPTIONS ${DEFAULT_COMPILE_OPTIONS}
		-fsized-deallocation
	)
endif ()

#
# Linker options
#

set(DEFAULT_LINKER_OPTIONS)

# Use pthreads on mingw and linux
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_SYSTEM_NAME MATCHES "Linux")
	set(DEFAULT_LINKER_OPTIONS
		-pthread
		-lstdc++fs
	)
endif()

# Code coverage - Debug only
# NOTE: Code coverage results with an optimized (non-Debug) build may be misleading
if (CMAKE_BUILD_TYPE MATCHES Debug AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set(DEFAULT_COMPILE_OPTIONS ${DEFAULT_COMPILE_OPTIONS}
        -g
        -O0
        -fprofile-arcs
        -ftest-coverage
    )

    set(DEFAULT_LINKER_OPTIONS ${DEFAULT_LINKER_OPTIONS}
        -fprofile-arcs
        -ftest-coverage
    )
endif()
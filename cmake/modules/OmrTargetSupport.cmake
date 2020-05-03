###############################################################################
# Copyright (c) 2020, 2020 IBM Corp. and others
#
# This program and the accompanying materials are made available under
# the terms of the Eclipse Public License 2.0 which accompanies this
# distribution and is available at http://eclipse.org/legal/epl-2.0
# or the Apache License, Version 2.0 which accompanies this distribution
# and is available at https://www.apache.org/licenses/LICENSE-2.0.
#
# This Source Code may also be made available under the following Secondary
# Licenses when the conditions for such availability set forth in the
# Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
# version 2 with the GNU Classpath Exception [1] and GNU General Public
# License, version 2 with the OpenJDK Assembly Exception [2].
#
# [1] https://www.gnu.org/software/classpath/license.html
# [2] http://openjdk.java.net/legal/assembly-exception.html
#
# SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
#############################################################################

if(OMR_TARGET_SUPPORT)
	return()
endif()
set(OMR_TARGET_SUPPORT 1)

include(OmrAssert)
include(OmrUtility)

# omr_add_library(name <sources> ...)
#   STATIC | SHARED | OBJECT  - same meanings as for standard add_library
# At present, a thin wrapper around add_library, but it ensures that exports
# and split debug info are handled
function(omr_add_library name)
	set(options SHARED STATIC OBJECT INTERFACE)
	set(oneValueArgs )
	set(multiValueArgs )

	foreach(var IN LISTS options oneValueArgs multiValueArgs)
		set(opt_${var} "")
	endforeach()
	cmake_parse_arguments(opt "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})


	omr_count_true(num_types VARIABLES opt_SHARED opt_STATIC opt_OBJECT opt_INTERFACE)
	omr_assert(TEST num_types LESS 2 MESSAGE "Only one of STATIC | SHARED | OBJECT | INTERFACE may be given")

	# if no target type given, fall back to STATIC|SHARED depending on BUILD_SHARED_LIBS
	# (same behavior as CMake)
	if(num_types EQUAL 0)
		if(BUILD_SHARED_LIBS)
			set(opt_SHARED True)
		else()
			set(opt_STATIC True)
		endif()
	endif()

	if(opt_SHARED)
		set(lib_type "SHARED")
	elseif(opt_STATIC)
		set(lib_type "STATIC")
	elseif(opt_OBJECT)
		set(lib_type "OBJECT")
	elseif(opt_INTERFACE)
		set(lib_type "INTERFACE")
	else()
		omr_assert(TEST "False" MESSAGE "Unreachable")
	endif()

	add_library(${name} ${lib_type} ${opt_UNPARSED_ARGUMENTS})

	if(opt_SHARED)
		# split debug info if applicable. Note: omr_split_debug is responsible for checking OMR_SEPARATE_DEBUG_INFO
		omr_process_split_debug(${name})
	endif()
endfunction()


# omr_add_executable(name <sources> ...)
# At present, a thin wrapper around add_executable, but it ensures that
# split debug info is handled
function(omr_add_executable name)
	add_executable(${name} ${ARGN})
	omr_process_split_debug(${name})
endfunction()

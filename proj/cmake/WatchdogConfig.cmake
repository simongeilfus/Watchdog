if( NOT TARGET Watchdog )
	get_filename_component( WATCHDOG_SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../include" ABSOLUTE )
	get_filename_component( CINDER_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE )

	add_library( Watchdog ${WATCHDOG_SOURCE_PATH}/Watchdog.h )

	target_include_directories( Watchdog PUBLIC "${WATCHDOG_SOURCE_PATH}" )
	target_include_directories( Watchdog SYSTEM BEFORE PUBLIC "${CINDER_PATH}/include" )

	if( NOT TARGET cinder )
		    include( "${CINDER_PATH}/proj/cmake/configure.cmake" )
		    find_package( cinder REQUIRED PATHS
		        "${CINDER_PATH}/${CINDER_LIB_DIRECTORY}"
		        "$ENV{CINDER_PATH}/${CINDER_LIB_DIRECTORY}" )
	endif()

	target_link_libraries( Watchdog PRIVATE cinder )

endif()

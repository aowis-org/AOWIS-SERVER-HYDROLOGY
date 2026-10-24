cmake_minimum_required(VERSION 3.21)
project(AOWIS_SERVER_HYDROLOGY VERSION 0.1.0 LANGUAGES CXX)
include(CTest)

# GeographicLib 2.7 is also used by AOWIS-SERVER-GUI. The source is normally
# installed as an actual Git submodule using tools/setup_geographiclib.sh.
# In parent meta-builds reuse the already-defined GeographicLib target.
set(AOWIS_HYDROLOGY_GEOGRAPHICLIB_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/external/GeographicLib" CACHE PATH
    "GeographicLib source directory (default: Hydrology Git submodule)")
set(AOWIS_HYDROLOGY_VENDORED_GEOGRAPHICLIB OFF)
if(NOT TARGET GeographicLib::GeographicLib AND NOT TARGET GeographicLib::GeographicLib_STATIC)
    if(EXISTS "${AOWIS_HYDROLOGY_GEOGRAPHICLIB_SOURCE}/CMakeLists.txt")
        # Build the vendored copy as a static library only. GeographicLib's own
        # option(BUILD_SHARED_LIBS ON) would otherwise create a global cache
        # entry, link the executables against a build-tree .so and leak that
        # default into a parent meta-build. Normal variables take precedence
        # over option() (CMP0077 NEW in GeographicLib's 3.17 policy scope),
        # and remain local to this directory scope.
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_BOTH_LIBS OFF)
        # Embedder warning flags must never turn into -Werror on vendored code.
        set(CONVERT_WARNINGS_TO_ERRORS OFF)
        set(BUILD_MANPAGES OFF)
        add_subdirectory("${AOWIS_HYDROLOGY_GEOGRAPHICLIB_SOURCE}"
            "${CMAKE_CURRENT_BINARY_DIR}/geographiclib" EXCLUDE_FROM_ALL)
        set(AOWIS_HYDROLOGY_VENDORED_GEOGRAPHICLIB ON)
    else()
        find_package(GeographicLib CONFIG QUIET)
        if(NOT TARGET GeographicLib::GeographicLib)
            message(FATAL_ERROR
                "GeographicLib is required. Run ./tools/setup_geographiclib.sh "
                "or set AOWIS_HYDROLOGY_GEOGRAPHICLIB_SOURCE to an initialized GeographicLib source tree.")
        endif()
    endif()
endif()
# Prefer the static library whenever it is available, also when a parent
# build already defined GeographicLib. The alias GeographicLib::GeographicLib
# may resolve to a shared library in such builds.
if(TARGET GeographicLib::GeographicLib_STATIC)
    set(AOWIS_HYDROLOGY_GEOGRAPHICLIB_TARGET GeographicLib::GeographicLib_STATIC)
else()
    set(AOWIS_HYDROLOGY_GEOGRAPHICLIB_TARGET GeographicLib::GeographicLib)
endif()

add_library(aowis-hydrology-core STATIC
    src/core/analysis.cpp
    src/core/flow_network.cpp
    src/core/spatial_queries.cpp
    src/core/geometry.cpp
    src/core/depression_hierarchy.cpp
    src/core/depression_archive.cpp
    src/core/rainfall.cpp
    src/core/redistribution.cpp
    src/core/spill_geometry.cpp
    src/core/spill_hydraulics.cpp
    src/core/coupled_routing.cpp
    src/core/temporal_inflows.cpp
    src/core/travel_time_inflows.cpp
    src/core/redistribution_archive.cpp
    src/core/inundation.cpp
    include/aowis/hydrology/rainfall.h
    include/aowis/hydrology/redistribution.h
    include/aowis/hydrology/spill_geometry.h
    include/aowis/hydrology/spill_hydraulics.h
    include/aowis/hydrology/coupled_routing.h
    include/aowis/hydrology/temporal_inflows.h
    include/aowis/hydrology/travel_time_inflows.h
    include/aowis/hydrology/redistribution_archive.h
    include/aowis/hydrology/inundation.h
    include/aowis/hydrology/depression_archive.h
    include/aowis/hydrology/depression_hierarchy.h
    include/aowis/hydrology/geometry.h
    include/aowis/hydrology/analysis.h
    include/aowis/hydrology/flow_network.h
    include/aowis/hydrology/spatial_queries.h
)
add_library(AOWIS::Hydrology ALIAS aowis-hydrology-core)
target_compile_features(aowis-hydrology-core PUBLIC cxx_std_20)
target_link_libraries(aowis-hydrology-core PUBLIC ${AOWIS_HYDROLOGY_GEOGRAPHICLIB_TARGET})
target_include_directories(aowis-hydrology-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Local, analysis-grade DEM mosaics are independent of Map and Qt. Keep GDAL
# optional for embedders which only use Map terrain or a custom terrain_reader.
option(AOWIS_HYDROLOGY_WITH_GDAL "Enable local read-only GeoTIFF mosaics via GDAL" ON)
if(AOWIS_HYDROLOGY_WITH_GDAL)
    find_package(GDAL REQUIRED)
    add_library(aowis-hydrology-raster STATIC
        src/raster/geotiff_source.cpp
        include/aowis/hydrology/geotiff_source.h
    )
    add_library(AOWIS::HydrologyRaster ALIAS aowis-hydrology-raster)
    target_compile_features(aowis-hydrology-raster PUBLIC cxx_std_20)
    target_include_directories(aowis-hydrology-raster PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(aowis-hydrology-raster PUBLIC AOWIS::Hydrology GDAL::GDAL)
endif()

# Benchmarks are deliberately opt-in: no synthetic million-cell workload is
# included in ordinary service builds or the default regression suite.
option(AOWIS_HYDROLOGY_BUILD_BENCHMARKS "Build reproducible synthetic Hydrology benchmarks" OFF)
if(AOWIS_HYDROLOGY_BUILD_BENCHMARKS)
    add_executable(aowis-hydrology-benchmark tools/benchmark_hydrology.cpp)
    target_compile_features(aowis-hydrology-benchmark PRIVATE cxx_std_20)
    target_link_libraries(aowis-hydrology-benchmark PRIVATE AOWIS::Hydrology)
    if(BUILD_TESTING)
        add_test(NAME aowis-hydrology-benchmark-smoke COMMAND aowis-hydrology-benchmark
            --columns 32 --rows 24 --seed 17 --terrain pitted)
        set_tests_properties(aowis-hydrology-benchmark-smoke PROPERTIES LABELS "hydrology;aowis;benchmark-smoke")
        add_test(NAME aowis-hydrology-benchmark-input-limits COMMAND aowis-hydrology-benchmark
            --columns 5000 --rows 5000)
        set_tests_properties(aowis-hydrology-benchmark-input-limits PROPERTIES
            LABELS "hydrology;aowis;benchmark-smoke" WILL_FAIL TRUE)
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME aowis-hydrology-benchmark-map-http-mock
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_map_http.py")
            set_tests_properties(aowis-hydrology-benchmark-map-http-mock PROPERTIES
                LABELS "hydrology;aowis;benchmark-smoke")
            add_test(NAME aowis-hydrology-benchmark-map-series-mock
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_map_series.py")
            set_tests_properties(aowis-hydrology-benchmark-map-series-mock PROPERTIES
                LABELS "hydrology;aowis;benchmark-smoke")
            add_test(NAME aowis-hydrology-benchmark-map-concurrency-mock
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_map_concurrency.py")
            set_tests_properties(aowis-hydrology-benchmark-map-concurrency-mock PROPERTIES
                LABELS "hydrology;aowis;benchmark-smoke")
            add_test(NAME aowis-hydrology-benchmark-map-jobs-mock
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_map_jobs.py")
            set_tests_properties(aowis-hydrology-benchmark-map-jobs-mock PROPERTIES
                LABELS "hydrology;aowis;benchmark-smoke")
        endif()
    endif()
endif()

option(AOWIS_HYDROLOGY_BUILD_SERVER "Build the cpp-httplib REST hydrology server" ON)
option(AOWIS_HYDROLOGY_BUILD_SERVICE "Build the reusable Qt job service (no HTTP server dependency)" ${AOWIS_HYDROLOGY_BUILD_SERVER})
option(AOWIS_HYDROLOGY_USE_LOCAL_MAP "Use the AOWIS Map C++ library for embedded terrain access" OFF)
set(AOWIS_HYDROLOGY_MAP_SOURCE "" CACHE PATH "Source directory of AOWIS-SERVER-MAP")

if(AOWIS_HYDROLOGY_BUILD_SERVER AND NOT AOWIS_HYDROLOGY_BUILD_SERVICE)
    message(FATAL_ERROR "The REST adapter requires AOWIS_HYDROLOGY_BUILD_SERVICE=ON")
endif()

if(AOWIS_HYDROLOGY_BUILD_SERVICE)
    find_package(Qt6 REQUIRED COMPONENTS Core Network)
    add_library(aowis-hydrology-service STATIC
        include/aowis/hydrology/service.h
        include/aowis/hydrology/terrain_source.h
        src/service/service.cpp
        src/service/catchment_tasks.cpp
        src/service/coupled_simulation.cpp
        src/service/terrain_source.cpp
        src/service/service_limits.h
    )
    add_library(AOWIS::HydrologyService ALIAS aowis-hydrology-service)
    target_compile_features(aowis-hydrology-service PUBLIC cxx_std_20)
    target_include_directories(aowis-hydrology-service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(aowis-hydrology-service PUBLIC AOWIS::Hydrology Qt6::Core Qt6::Network)
    if(AOWIS_HYDROLOGY_USE_LOCAL_MAP)
        if(NOT TARGET AOWIS::Map)
            if(NOT AOWIS_HYDROLOGY_MAP_SOURCE)
                message(FATAL_ERROR "Add AOWIS-SERVER-MAP first or set AOWIS_HYDROLOGY_MAP_SOURCE")
            endif()
            set(AOWIS_SERVER_MAP_BUILD_SERVER OFF CACHE BOOL "" FORCE)
            add_subdirectory("${AOWIS_HYDROLOGY_MAP_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/aowis-map")
        endif()
        target_link_libraries(aowis-hydrology-service PUBLIC AOWIS::Map)
        target_compile_definitions(aowis-hydrology-service PUBLIC AOWIS_HYDROLOGY_LOCAL_MAP=1)
    endif()
endif()

if(AOWIS_HYDROLOGY_BUILD_SERVER)
    set(AOWIS_HYDROLOGY_HTTPLIB_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/external/cpp-httplib" CACHE PATH
        "Directory containing pinned cpp-httplib httplib.h")
    if(NOT EXISTS "${AOWIS_HYDROLOGY_HTTPLIB_SOURCE}/httplib.h")
        message(FATAL_ERROR "cpp-httplib is missing. Run ./tools/setup_cpp_httplib.sh before configuring the server")
    endif()
    find_package(Threads REQUIRED)
    add_executable(aowis-server-hydrology
        src/server/main.cpp
        src/server/server.cpp
        src/server/server.h
        src/server/http_adapter.h
        src/server/http_status.h
    )
    target_compile_features(aowis-server-hydrology PRIVATE cxx_std_20)
    target_link_libraries(aowis-server-hydrology PRIVATE
        AOWIS::HydrologyService Threads::Threads)
    target_include_directories(aowis-server-hydrology PRIVATE
        "${AOWIS_HYDROLOGY_HTTPLIB_SOURCE}")
    if(BUILD_TESTING)
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME aowis-hydrology-server-config-cli
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_server_config_cli.py"
                    "$<TARGET_FILE:aowis-server-hydrology>")
            set_tests_properties(aowis-hydrology-server-config-cli PROPERTIES
                LABELS "hydrology;aowis")
            # API compatibility gate for HTTP-adapter replacements. This launches
            # the real server with an isolated persistent directory and a tiny
            # deterministic mock of Map's analysis-grade DEM endpoint.
            add_test(NAME aowis-hydrology-http-contract
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_http_contract.py"
                    "$<TARGET_FILE:aowis-server-hydrology>")
            set_tests_properties(aowis-hydrology-http-contract PROPERTIES
                LABELS "hydrology;aowis;http-contract" TIMEOUT 120 RUN_SERIAL TRUE)
        endif()
    endif()
endif()

if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME aowis-hydrology-prune-safety
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prune_hydrology.py")
        set_tests_properties(aowis-hydrology-prune-safety PROPERTIES
            LABELS "hydrology;aowis")
        add_test(NAME aowis-hydrology-resource-benchmark-tool
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_resource_scaling.py")
        set_tests_properties(aowis-hydrology-resource-benchmark-tool PROPERTIES
            LABELS "hydrology;aowis")
        add_test(NAME aowis-hydrology-derived-cache-benchmark-tool
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_derived_caches.py")
        set_tests_properties(aowis-hydrology-derived-cache-benchmark-tool PROPERTIES
            LABELS "hydrology;aowis;http-contract")
        add_test(NAME aowis-hydrology-documentation-links
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_documentation_links.py")
        set_tests_properties(aowis-hydrology-documentation-links PROPERTIES
            LABELS "hydrology;aowis")
    endif()
endif()

if(BUILD_TESTING)
    add_executable(aowis-hydrology-core-test tests/test_analysis.cpp)
    target_link_libraries(aowis-hydrology-core-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-core COMMAND aowis-hydrology-core-test)
    set_tests_properties(aowis-hydrology-core PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-flow-network-test tests/test_flow_network.cpp)
    target_link_libraries(aowis-hydrology-flow-network-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-flow-network COMMAND aowis-hydrology-flow-network-test)
    set_tests_properties(aowis-hydrology-flow-network PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-spatial-queries-test tests/test_spatial_queries.cpp)
    target_link_libraries(aowis-hydrology-spatial-queries-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-spatial-queries COMMAND aowis-hydrology-spatial-queries-test)
    set_tests_properties(aowis-hydrology-spatial-queries PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-depressions-test tests/test_depression_hierarchy.cpp)
    target_link_libraries(aowis-hydrology-depressions-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-depressions COMMAND aowis-hydrology-depressions-test)
    set_tests_properties(aowis-hydrology-depressions PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-rainfall-test tests/test_rainfall.cpp)
    target_link_libraries(aowis-hydrology-rainfall-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-rainfall COMMAND aowis-hydrology-rainfall-test)
    set_tests_properties(aowis-hydrology-rainfall PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-redistribution-test tests/test_redistribution.cpp)
    target_link_libraries(aowis-hydrology-redistribution-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-redistribution COMMAND aowis-hydrology-redistribution-test)
    set_tests_properties(aowis-hydrology-redistribution PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-sibling-allocation-test tests/test_sibling_allocation.cpp)
    target_link_libraries(aowis-hydrology-sibling-allocation-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-sibling-allocation COMMAND aowis-hydrology-sibling-allocation-test)
    set_tests_properties(aowis-hydrology-sibling-allocation PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-spill-geometry-test tests/test_spill_geometry.cpp)
    target_link_libraries(aowis-hydrology-spill-geometry-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-spill-geometry COMMAND aowis-hydrology-spill-geometry-test)
    set_tests_properties(aowis-hydrology-spill-geometry PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-spill-hydraulics-test tests/test_spill_hydraulics.cpp)
    target_link_libraries(aowis-hydrology-spill-hydraulics-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-spill-hydraulics COMMAND aowis-hydrology-spill-hydraulics-test)
    set_tests_properties(aowis-hydrology-spill-hydraulics PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-submerged-controls-test tests/test_submerged_controls.cpp)
    target_link_libraries(aowis-hydrology-submerged-controls-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-submerged-controls COMMAND aowis-hydrology-submerged-controls-test)
    set_tests_properties(aowis-hydrology-submerged-controls PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-coupled-routing-test tests/test_coupled_routing.cpp)
    target_link_libraries(aowis-hydrology-coupled-routing-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-coupled-routing COMMAND aowis-hydrology-coupled-routing-test)
    set_tests_properties(aowis-hydrology-coupled-routing PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-terrain-reference-test tests/test_terrain_reference.cpp)
    target_link_libraries(aowis-hydrology-terrain-reference-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-terrain-reference COMMAND aowis-hydrology-terrain-reference-test)
    set_tests_properties(aowis-hydrology-terrain-reference PROPERTIES LABELS "hydrology;aowis;numerical-verification")

    add_executable(aowis-hydrology-coupled-convergence-test tests/test_coupled_convergence.cpp)
    target_link_libraries(aowis-hydrology-coupled-convergence-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-coupled-convergence COMMAND aowis-hydrology-coupled-convergence-test)
    set_tests_properties(aowis-hydrology-coupled-convergence PROPERTIES LABELS "hydrology;aowis;numerical-verification")

    add_executable(aowis-hydrology-boundary-outlets-test tests/test_boundary_outlets.cpp)
    target_link_libraries(aowis-hydrology-boundary-outlets-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-boundary-outlets COMMAND aowis-hydrology-boundary-outlets-test)
    set_tests_properties(aowis-hydrology-boundary-outlets PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-temporal-inflows-test tests/test_temporal_inflows.cpp)
    target_link_libraries(aowis-hydrology-temporal-inflows-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-temporal-inflows COMMAND aowis-hydrology-temporal-inflows-test)
    set_tests_properties(aowis-hydrology-temporal-inflows PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-travel-time-test tests/test_travel_time_inflows.cpp)
    target_link_libraries(aowis-hydrology-travel-time-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-travel-time COMMAND aowis-hydrology-travel-time-test)
    set_tests_properties(aowis-hydrology-travel-time PROPERTIES LABELS "hydrology;aowis")

    add_executable(aowis-hydrology-inundation-test tests/test_inundation.cpp)
    target_link_libraries(aowis-hydrology-inundation-test PRIVATE AOWIS::Hydrology)
    add_test(NAME aowis-hydrology-inundation COMMAND aowis-hydrology-inundation-test)
    set_tests_properties(aowis-hydrology-inundation PROPERTIES LABELS "hydrology;aowis")

    if(AOWIS_HYDROLOGY_WITH_GDAL)
        add_executable(aowis-hydrology-geotiff-test tests/test_geotiff.cpp)
        target_link_libraries(aowis-hydrology-geotiff-test PRIVATE AOWIS::HydrologyRaster)
        add_test(NAME aowis-hydrology-geotiff COMMAND aowis-hydrology-geotiff-test)
        set_tests_properties(aowis-hydrology-geotiff PROPERTIES LABELS "hydrology;aowis")
    endif()

    if(AOWIS_HYDROLOGY_BUILD_SERVICE)
        add_executable(aowis-hydrology-service-test tests/test_service.cpp)
        target_link_libraries(aowis-hydrology-service-test PRIVATE AOWIS::HydrologyService)
        add_test(NAME aowis-hydrology-service COMMAND aowis-hydrology-service-test)
        set_tests_properties(aowis-hydrology-service PROPERTIES LABELS "hydrology;aowis")

        add_executable(aowis-hydrology-http-status-test tests/test_http_status.cpp)
        target_link_libraries(aowis-hydrology-http-status-test PRIVATE Qt6::Core)
        add_test(NAME aowis-hydrology-http-status COMMAND aowis-hydrology-http-status-test)
        set_tests_properties(aowis-hydrology-http-status PROPERTIES LABELS "hydrology;aowis")

        add_executable(aowis-hydrology-terrain-transport-test tests/test_terrain_transport.cpp)
        target_link_libraries(aowis-hydrology-terrain-transport-test PRIVATE AOWIS::HydrologyService)
        add_test(NAME aowis-hydrology-terrain-transport COMMAND aowis-hydrology-terrain-transport-test)
        set_tests_properties(aowis-hydrology-terrain-transport PROPERTIES LABELS "hydrology;aowis")
    endif()
endif()

# GeographicLib unconditionally calls enable_testing()/add_test() for its own
# tool programs, which are excluded from the default build. Without this, a
# plain `ctest` reports hundreds of "Not Run" failures. CTest reads
# CTestCustom.cmake from the top-level build directory; list exactly the tests
# registered by the vendored GeographicLib directory so they are skipped.
if(BUILD_TESTING AND AOWIS_HYDROLOGY_VENDORED_GEOGRAPHICLIB)
    get_property(AOWIS_HYDROLOGY_GEOGRAPHICLIB_TESTS
        DIRECTORY "${AOWIS_HYDROLOGY_GEOGRAPHICLIB_SOURCE}/tests" PROPERTY TESTS)
    if(AOWIS_HYDROLOGY_GEOGRAPHICLIB_TESTS)
        string(REPLACE ";" "\"\n    \"" AOWIS_HYDROLOGY_IGNORED_TESTS
               "${AOWIS_HYDROLOGY_GEOGRAPHICLIB_TESTS}")
        file(WRITE "${CMAKE_BINARY_DIR}/CTestCustom.cmake"
            "# Generated by AOWIS-SERVER-HYDROLOGY: skip vendored GeographicLib tests.\n"
            "list(APPEND CTEST_CUSTOM_TESTS_IGNORE\n    \"${AOWIS_HYDROLOGY_IGNORED_TESTS}\"\n)\n")
    endif()
endif()

# Deployment: `cmake --install <build> --prefix <dir>` installs the server
# executable only. GeographicLib is linked statically, so the executable does
# not depend on any library inside the build tree. Disabled by default when
# Hydrology is a subproject so a parent build keeps control of installation.
option(AOWIS_HYDROLOGY_INSTALL "Install the Hydrology server executable"
       ${PROJECT_IS_TOP_LEVEL})
if(AOWIS_HYDROLOGY_INSTALL AND AOWIS_HYDROLOGY_BUILD_SERVER)
    include(GNUInstallDirs)
    install(TARGETS aowis-server-hydrology
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

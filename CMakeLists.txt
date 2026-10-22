cmake_minimum_required(VERSION 3.21)
project(AOWIS_SERVER_HYDROLOGY VERSION 0.1.0 LANGUAGES CXX)
include(CTest)

# GeographicLib 2.7 is also used by AOWIS-SERVER-GUI. The source is normally
# installed as an actual Git submodule using tools/setup_geographiclib.sh.
# In parent meta-builds reuse the already-defined GeographicLib target.
set(AOWIS_HYDROLOGY_GEOGRAPHICLIB_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/external/GeographicLib" CACHE PATH
    "GeographicLib source directory (default: Hydrology Git submodule)")
if(NOT TARGET GeographicLib::GeographicLib)
    if(EXISTS "${AOWIS_HYDROLOGY_GEOGRAPHICLIB_SOURCE}/CMakeLists.txt")
        add_subdirectory("${AOWIS_HYDROLOGY_GEOGRAPHICLIB_SOURCE}"
            "${CMAKE_CURRENT_BINARY_DIR}/geographiclib" EXCLUDE_FROM_ALL)
    else()
        find_package(GeographicLib CONFIG QUIET)
        if(NOT TARGET GeographicLib::GeographicLib)
            message(FATAL_ERROR
                "GeographicLib is required. Run ./tools/setup_geographiclib.sh "
                "or set AOWIS_HYDROLOGY_GEOGRAPHICLIB_SOURCE to an initialized GeographicLib source tree.")
        endif()
    endif()
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
target_link_libraries(aowis-hydrology-core PUBLIC GeographicLib::GeographicLib)
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
        endif()
    endif()
endif()

option(AOWIS_HYDROLOGY_BUILD_SERVER "Build the Qt REST hydrology server" ON)
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
        src/service/coupled_simulation.cpp
        src/service/terrain_source.cpp
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
    find_package(Qt6 REQUIRED COMPONENTS HttpServer)
    add_executable(aowis-server-hydrology
        src/server/main.cpp
        src/server/server.cpp
        src/server/server.h
    )
    target_compile_features(aowis-server-hydrology PRIVATE cxx_std_20)
    target_link_libraries(aowis-server-hydrology PRIVATE
        AOWIS::HydrologyService Qt6::HttpServer)
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

        add_executable(aowis-hydrology-terrain-transport-test tests/test_terrain_transport.cpp)
        target_link_libraries(aowis-hydrology-terrain-transport-test PRIVATE AOWIS::HydrologyService)
        add_test(NAME aowis-hydrology-terrain-transport COMMAND aowis-hydrology-terrain-transport-test)
        set_tests_properties(aowis-hydrology-terrain-transport PROPERTIES LABELS "hydrology;aowis")
    endif()
endif()

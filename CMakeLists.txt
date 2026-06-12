cmake_minimum_required(VERSION 3.20)

project(ChronosX
    VERSION 0.1.0
    DESCRIPTION "Chronos-X deterministic network chaos engineering tool"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

option(CHRONOSX_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CHRONOSX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(CHRONOSX_NATIVE_OPTIMIZATIONS "Enable native CPU tuning for release builds" OFF)
option(CHRONOSX_ENABLE_LTO "Enable link-time optimization for release builds" OFF)
option(CHRONOSX_BUILD_BPF "Build the XDP/eBPF object with clang -target bpf" OFF)
option(CHRONOSX_ENABLE_LIBXDP "Enable the real libxdp AF_XDP socket backend" OFF)

if(CHRONOSX_ENABLE_LIBXDP)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBXDP REQUIRED libxdp)
    pkg_check_modules(LIBBPF REQUIRED libbpf)
endif()

function(chronosx_configure_target target_name)
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(${target_name}
        PRIVATE
            Threads::Threads
    )

    if(CHRONOSX_ENABLE_LIBXDP)
        target_compile_definitions(${target_name}
            PRIVATE
                CHRONOSX_ENABLE_LIBXDP=1
        )
        target_include_directories(${target_name}
            PRIVATE
                ${LIBXDP_INCLUDE_DIRS}
                ${LIBBPF_INCLUDE_DIRS}
        )
        target_link_libraries(${target_name}
            PRIVATE
                ${LIBXDP_LIBRARIES}
                ${LIBBPF_LIBRARIES}
        )
        target_link_directories(${target_name}
            PRIVATE
                ${LIBXDP_LIBRARY_DIRS}
                ${LIBBPF_LIBRARY_DIRS}
        )
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
        )

        if(CHRONOSX_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name}
                PRIVATE
                    -Werror
            )
        endif()

        target_compile_options(${target_name}
            PRIVATE
                $<$<CONFIG:Release>:-O3>
        )

        if(CHRONOSX_NATIVE_OPTIMIZATIONS)
            target_compile_options(${target_name}
                PRIVATE
                    $<$<CONFIG:Release>:-march=native>
            )
        endif()

        if(CHRONOSX_ENABLE_LTO)
            include(CheckIPOSupported)
            check_ipo_supported(RESULT chronosx_ipo_supported OUTPUT chronosx_ipo_error)
            if(chronosx_ipo_supported)
                set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
            else()
                message(WARNING "IPO/LTO not supported: ${chronosx_ipo_error}")
            endif()
        endif()
    endif()

    if(CHRONOSX_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name}
            PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
        )
        target_link_options(${target_name}
            PRIVATE
                -fsanitize=address,undefined
        )
    endif()
endfunction()

function(chronosx_add_test ctest_name target_name source_file)
    add_executable(${target_name}
        ${source_file}
    )

    chronosx_configure_target(${target_name})

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name}
            PRIVATE
                -UNDEBUG
        )
    endif()

    add_test(NAME ${ctest_name} COMMAND ${target_name})
endfunction()

add_executable(chronosx
    src/main.cpp
)

chronosx_configure_target(chronosx)

add_executable(bench_data_plane
    benchmarks/bench_data_plane.cpp
)

chronosx_configure_target(bench_data_plane)

if(CHRONOSX_BUILD_BPF)
    find_program(CLANG_BPF clang REQUIRED)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdp_redirect.bpf.o
        COMMAND ${CLANG_BPF} -target bpf -O2 -g
                -I/usr/include/${CMAKE_SYSTEM_PROCESSOR}-linux-gnu
                -c ${CMAKE_CURRENT_SOURCE_DIR}/bpf/xdp_redirect.bpf.c
                -o ${CMAKE_CURRENT_BINARY_DIR}/xdp_redirect.bpf.o
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bpf/xdp_redirect.bpf.c
        VERBATIM
    )
    add_custom_target(chronosx_bpf ALL
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/xdp_redirect.bpf.o
    )
endif()

enable_testing()

chronosx_add_test(spsc_queue test_spsc_queue tests/test_spsc_queue.cpp)
chronosx_add_test(types_layout test_types_layout tests/test_types_layout.cpp)
chronosx_add_test(packet_parser test_packet_parser tests/test_packet_parser.cpp)
chronosx_add_test(chaos_engine test_chaos_engine tests/test_chaos_engine.cpp)
chronosx_add_test(timing_wheel test_timing_wheel tests/test_timing_wheel.cpp)
chronosx_add_test(frame_allocator test_frame_allocator tests/test_frame_allocator.cpp)
chronosx_add_test(data_plane test_data_plane tests/test_data_plane.cpp)
chronosx_add_test(protocol test_protocol tests/test_protocol.cpp)
chronosx_add_test(socket_manager test_socket_manager tests/test_socket_manager.cpp)
chronosx_add_test(control_plane test_control_plane tests/test_control_plane.cpp)
chronosx_add_test(af_xdp_runtime test_af_xdp_runtime tests/test_af_xdp_runtime.cpp)
if(CHRONOSX_ENABLE_LIBXDP)
    chronosx_add_test(libxdp_socket test_libxdp_socket tests/test_libxdp_socket.cpp)
endif()
chronosx_add_test(stress test_stress tests/test_stress.cpp)

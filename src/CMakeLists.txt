if (DEFINED ACL_PATH)
        message(STATUS "user set ACL_PATH: ${ACL_PATH}")
elseif (EXISTS /usr/local/Ascend/acllib/)
        set(ACL_PATH "/usr/local/Ascend/acllib")
        message(STATUS "set ACL_PATH: /usr/local/Ascend/acllib")
elseif (EXISTS /usr/local/Ascend/ascend-toolkit/latest/acllib)
        set(ACL_PATH "/usr/local/Ascend/ascend-toolkit/latest/acllib")
        message(STATUS "set ACL_PATH to default path: /usr/local/Ascend/ascend-toolkit/latest/acllib")
elseif (EXISTS /usr/local/Ascend/nnrt/latest/acllib)
        set(ACL_PATH "/usr/local/Ascend/nnrt/latest/acllib")
        message(STATUS "set ACL_PATH to default path: /usr/local/Ascend/nnrt/latest/acllib")    
else ()
        set(ACL_PATH "/home/HwHiAiUser/Ascend/acllib")
        message(STATUS "set ACL_PATH to default path: /home/HwHiAiUser/Ascend/acllib")    
endif()

set(Python_FIND_VIRTUALENV FIRST)

find_program(Python_EXECUTABLE
    NAMES python3 python
    DOC "Path to Python executable"
)
if(NOT Python_EXECUTABLE)
    message(FATAL_ERROR "Python executable not found in PATH!")
endif()


find_package(Python REQUIRED COMPONENTS Interpreter  Development)
if(NOT Python_FOUND)
    message(FATAL_ERROR "Python development libraries not found")
endif()
message("using python: ${Python_EXECUTABLE}")
message("using python lib: ${Python_LIBRARY_DIRS}")

find_package (Eigen3 REQUIRED NO_MODULE)

set(COMMON_SOURCE_FILES
        device.cpp
        device_cpu.cpp
        device_gpu.cpp
        device_npu.cpp
        llama2_layer_cpu.cpp
        llama2_layer_npu.cpp
        model_base.cpp
        profiling.cpp
        qwen2_model.cpp
        tokenizer.cpp
        util.cpp
)

add_executable(qwen_openai_server openai_server_main.cpp ${COMMON_SOURCE_FILES})
message("${EIGEN3_INCLUDE_DIR}")
target_include_directories(qwen_openai_server PUBLIC ${EIGEN3_INCLUDE_DIR} ${ACL_PATH}/include ../prebuild ${Python_INCLUDE_DIRS})
target_link_directories(qwen_openai_server PUBLIC ${ACL_PATH}/lib64 ../prebuild ${Python_LIBRARY_DIRS})
target_link_libraries(qwen_openai_server sentencepiece boost_program_options boost_system boost_filesystem fmt ascendcl runtime npu_ops re2 ${Python_LIBRARIES})

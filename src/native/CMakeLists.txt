cmake_minimum_required(VERSION 3.20)
project(linrtx_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_program(GLSLC_EXECUTABLE NAMES glslc REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

set(NGX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/nvidia-dlss")
set(GEN_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_SHADER_DIR}")

function(compile_comp_shader SHADER_PATH VAR_NAME OUT_HEADER)
    get_filename_component(BASE_NAME "${SHADER_PATH}" NAME_WE)
    set(SPV_OUT "${GEN_SHADER_DIR}/${BASE_NAME}.spv")
    add_custom_command(
        OUTPUT "${SPV_OUT}"
        COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.2 -O "${SHADER_PATH}" -o "${SPV_OUT}"
        DEPENDS "${SHADER_PATH}"
        COMMENT "Compiling SPIR-V compute shader ${BASE_NAME}.comp"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${OUT_HEADER}"
        COMMAND "${CMAKE_COMMAND}"
            -DINPUT_SPV=${SPV_OUT}
            -DOUTPUT_H=${OUT_HEADER}
            -DVAR_NAME=${VAR_NAME}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spv_to_header.cmake"
        DEPENDS "${SPV_OUT}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spv_to_header.cmake"
        COMMENT "Generating embedded SPIR-V header ${OUT_HEADER}"
        VERBATIM
    )
endfunction()

set(RTX_HDR_HEADER "${GEN_SHADER_DIR}/rtx_hdr_spv.h")
set(CAS_SHARPEN_HEADER "${GEN_SHADER_DIR}/cas_sharpen_spv.h")
set(SVGF_TEMPORAL_HEADER "${GEN_SHADER_DIR}/svgf_temporal_spv.h")
set(SVGF_ATROUS_HEADER "${GEN_SHADER_DIR}/svgf_atrous_spv.h")
set(GBUFFER_SYNTH_HEADER "${GEN_SHADER_DIR}/gbuffer_synth_spv.h")
set(COLOR_CONVERT_HEADER "${GEN_SHADER_DIR}/color_convert_spv.h")
set(NEURAL_ANALYSIS_HEADER "${GEN_SHADER_DIR}/neural_analysis_spv.h")
set(NEURAL_FILTER_HEADER "${GEN_SHADER_DIR}/neural_filter_spv.h")
set(GUIDE_COMPLETE_HEADER "${GEN_SHADER_DIR}/guide_complete_spv.h")
set(PRE_TONEMAP_RESOLVE_HEADER "${GEN_SHADER_DIR}/pre_tonemap_resolve_spv.h")

compile_comp_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/rtx_hdr.comp" "kRtxHdrSpv" "${RTX_HDR_HEADER}")
compile_comp_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/cas_sharpen.comp" "kCasSharpenSpv" "${CAS_SHARPEN_HEADER}")
compile_comp_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/svgf_temporal.comp" "kSvgfTemporalSpv" "${SVGF_TEMPORAL_HEADER}")
compile_comp_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/svgf_atrous.comp" "kSvgfAtrousSpv" "${SVGF_ATROUS_HEADER}")
compile_comp_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/gbuffer_synth.comp" "kGbufferSynthSpv" "${GBUFFER_SYNTH_HEADER}")
compile_comp_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/color_convert.comp" "kColorConvertSpv" "${COLOR_CONVERT_HEADER}")
compile_comp_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/neural_analysis.comp" "kNeuralAnalysisSpv" "${NEURAL_ANALYSIS_HEADER}")
compile_comp_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/neural_filter.comp" "kNeuralFilterSpv" "${NEURAL_FILTER_HEADER}")
compile_comp_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/guide_complete.comp" "kGuideCompleteSpv" "${GUIDE_COMPLETE_HEADER}")
compile_comp_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/pre_tonemap_resolve.comp" "kPreTonemapResolveSpv" "${PRE_TONEMAP_RESOLVE_HEADER}")

add_library(linrtx SHARED
    src/gl_dispatch.cpp
    src/vk_ngx_context.cpp
    src/compute_pipeline.cpp
    src/svgf_denoiser.cpp
    src/pre_tonemap_helper.cpp
    src/dlss_sr.cpp
    src/dlss_rr.cpp
    src/dlss_fg.cpp
    src/dlss_nr.cpp
    src/rtx_hdr.cpp
    src/vk_presenter.cpp
    src/linrtx_api.cpp
    "${RTX_HDR_HEADER}"
    "${CAS_SHARPEN_HEADER}"
    "${SVGF_TEMPORAL_HEADER}"
    "${SVGF_ATROUS_HEADER}"
    "${GBUFFER_SYNTH_HEADER}"
    "${COLOR_CONVERT_HEADER}"
    "${NEURAL_ANALYSIS_HEADER}"
    "${NEURAL_FILTER_HEADER}"
    "${GUIDE_COMPLETE_HEADER}"
    "${PRE_TONEMAP_RESOLVE_HEADER}"
)

target_include_directories(linrtx PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GEN_SHADER_DIR}"
)

target_include_directories(linrtx SYSTEM PRIVATE
    "${NGX_ROOT}/include"
)

target_compile_options(linrtx PRIVATE
    -Wall -Wextra -Wno-unused-parameter -O2
)

# Link official NGX Vulkan stub statically + Vulkan + dl (X11/Xext/GL are dlopen'd cleanly at runtime)
target_link_libraries(linrtx PRIVATE
    "${NGX_ROOT}/lib/libnvsdk_ngx.a"
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
    Threads::Threads
)

# Copy compiled shared library directly into Java resource folder on build
set(JAVA_NATIVE_RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../main/resources/natives/linux-x86_64")
add_custom_command(TARGET linrtx POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${JAVA_NATIVE_RES_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different $<TARGET_FILE:linrtx> "${JAVA_NATIVE_RES_DIR}/liblinrtx.so"
    COMMENT "Staging liblinrtx.so into src/main/resources/natives/linux-x86_64"
)

enable_testing()
add_executable(test_linrtx_abi tests/test_linrtx_abi.cpp)
target_include_directories(test_linrtx_abi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(test_linrtx_abi PRIVATE linrtx ${CMAKE_DL_LIBS})
add_test(NAME test_linrtx_abi COMMAND test_linrtx_abi)

add_executable(test_dlss_helpers tests/test_dlss_helpers.cpp)
target_include_directories(test_dlss_helpers PRIVATE
    $<TARGET_PROPERTY:linrtx,INCLUDE_DIRECTORIES>
)
target_link_libraries(test_dlss_helpers PRIVATE linrtx ${CMAKE_DL_LIBS})
add_test(NAME test_dlss_helpers COMMAND test_dlss_helpers)

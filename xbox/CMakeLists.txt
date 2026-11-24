# chiaki-ng for Xbox (UWP / Dev Mode)
cmake_minimum_required(VERSION 3.20)
project(chiaki-xbox C CXX)

# chiaki-ng sources: this folder lives inside the chiaki4xbox fork, so the repo root is one level up
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../lib/src/session.c")
	set(CHIAKI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
else()
	set(CHIAKI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../chiaki-ng")
endif()
set(TP "${CHIAKI_DIR}/third-party")

# UWP projects enable SDL checks (/sdl) which turn old C APIs into errors
add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
add_compile_options(/sdl- /wd4996 /wd4244 /wd4267)

find_package(OpenSSL REQUIRED)
find_package(Opus CONFIG REQUIRED)

# ---- nanopb ----
add_library(nanopb STATIC ${TP}/nanopb/pb_common.c ${TP}/nanopb/pb_decode.c ${TP}/nanopb/pb_encode.c)
target_include_directories(nanopb PUBLIC ${TP}/nanopb)
target_compile_definitions(nanopb PUBLIC PB_C99_STATIC_ASSERT)

# ---- gf-complete + jerasure ----
add_library(gf_complete STATIC
	${TP}/gf-complete/src/gf.c ${TP}/gf-complete/src/gf_wgen.c ${TP}/gf-complete/src/gf_w4.c
	${TP}/gf-complete/src/gf_w8.c ${TP}/gf-complete/src/gf_w16.c ${TP}/gf-complete/src/gf_w32.c
	${TP}/gf-complete/src/gf_w64.c ${TP}/gf-complete/src/gf_w128.c ${TP}/gf-complete/src/gf_rand.c
	${TP}/gf-complete/src/gf_general.c ${TP}/gf-complete/src/gf_cpu.c)
target_include_directories(gf_complete PUBLIC ${TP}/gf-complete/include)

add_library(jerasure STATIC
	${TP}/jerasure/src/galois.c ${TP}/jerasure/src/jerasure.c ${TP}/jerasure/src/reed_sol.c
	${TP}/jerasure/src/cauchy.c ${TP}/jerasure/src/liberation.c)
target_include_directories(jerasure PUBLIC ${TP}/jerasure/include)
target_link_libraries(jerasure PUBLIC gf_complete)

# ---- chiaki core library ----
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/libsrc.txt" CHIAKI_SRC_REL)
set(CHIAKI_SRC "")
foreach(s ${CHIAKI_SRC_REL})
	list(APPEND CHIAKI_SRC "${CHIAKI_DIR}/lib/${s}")
endforeach()

add_library(chiaki-lib STATIC ${CHIAKI_SRC}
	"${CMAKE_CURRENT_SOURCE_DIR}/gen/takion.pb.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/stubs/uwp_stubs.c")
target_include_directories(chiaki-lib PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CHIAKI_DIR}/lib/include"
	"${CMAKE_CURRENT_SOURCE_DIR}/gen")
target_compile_options(chiaki-lib PRIVATE /FImalloc.h)
target_include_directories(chiaki-lib PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
target_compile_definitions(chiaki-lib PUBLIC
	CHIAKI_VERSION_MAJOR=1 CHIAKI_VERSION_MINOR=0 CHIAKI_VERSION_PATCH=0 CHIAKI_VERSION="xbox"
	_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(chiaki-lib PUBLIC nanopb jerasure OpenSSL::Crypto Opus::opus
	ws2_32 bcrypt iphlpapi)

# ---- Xbox app (UWP) ----
set(APP_ASSETS
	app/Assets/Square44x44Logo.png app/Assets/Square150x150Logo.png app/Assets/Wide310x150Logo.png
	app/Assets/StoreLogo.png app/Assets/SplashScreen.png app/Assets/config.json app/Assets/ui_logo.png app/Assets/ui_ps5.png)
set_source_files_properties(${APP_ASSETS} PROPERTIES VS_DEPLOYMENT_CONTENT 1 VS_DEPLOYMENT_LOCATION "Assets")
# Ship the vcpkg DLLs (opus, OpenSSL) inside the app package
file(GLOB APP_DLLS "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-uwp/bin/*.dll")
set_source_files_properties(${APP_DLLS} PROPERTIES VS_DEPLOYMENT_CONTENT 1 VS_DEPLOYMENT_LOCATION ".")
# Frame generation compute shaders (compiled by fxc into headers at build time)
set(FG_SHADERS fg_luma fg_motion fg_smooth fg_interp fg_filter)
set(FG_SHADER_SRC "")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/shaders")
foreach(sh ${FG_SHADERS})
	set_source_files_properties(app/shaders/${sh}.hlsl PROPERTIES
		VS_SHADER_TYPE Compute VS_SHADER_MODEL 5.0 VS_SHADER_ENTRYPOINT main
		VS_SHADER_OUTPUT_HEADER_FILE "${CMAKE_BINARY_DIR}/shaders/${sh}.h"
		VS_SHADER_VARIABLE_NAME g_${sh})
	list(APPEND FG_SHADER_SRC app/shaders/${sh}.hlsl)
endforeach()
add_executable(ChiakiXbox WIN32 app/main.cpp app/Package.appxmanifest app/ChiakiXbox_TemporaryKey.pfx ${APP_ASSETS} ${APP_DLLS} ${FG_SHADER_SRC} app/shaders/fg_common.hlsli)
set_target_properties(ChiakiXbox PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON VS_WINRT_EXTENSIONS OFF)
target_compile_options(ChiakiXbox PRIVATE /EHsc /bigobj /permissive-)
target_include_directories(ChiakiXbox PRIVATE "${CMAKE_BINARY_DIR}/shaders")
target_link_libraries(ChiakiXbox PRIVATE chiaki-lib d3d11 dxgi d2d1 dwrite mfplat mfuuid xaudio2 windowsapp)


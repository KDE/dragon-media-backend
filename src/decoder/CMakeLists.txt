add_library(dragon_decoder_objs OBJECT
    dragondecoder.cpp
    dragondecodepipeline.cpp
)

target_include_directories(dragon_decoder_objs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}
)

target_link_libraries(dragon_decoder_objs PRIVATE
    PkgConfig::FFmpeg
    Qt6::Core
    QCoro6::Core
)

set_target_properties(dragon_decoder_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(dragon_decoder_objs PRIVATE DragonMultimedia_EXPORTS)
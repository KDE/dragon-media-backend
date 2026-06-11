add_library(dragon_stream_objs OBJECT
    dragonstream.cpp
    dragonstreamfactory.cpp
    dragonradiostream.cpp
    dragonkiostream.cpp
    dragonbufferprogress.cpp
    dragonicymetadata.cpp
)

target_include_directories(dragon_stream_objs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}
)

target_link_libraries(dragon_stream_objs PRIVATE
    Qt6::Core
    Qt6::Network
    KF6::KIOCore
    KF6::CoreAddons
    QCoro6::Core
    QCoro6::Network
)

set_target_properties(dragon_stream_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(dragon_stream_objs PRIVATE DragonMultimedia_EXPORTS)
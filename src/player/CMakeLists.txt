add_library(dragon_player_objs OBJECT
    dragonplayer.cpp
    dragonpipe.cpp
    dragondiagnostics.cpp
)

target_include_directories(dragon_player_objs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}
    ${PROJECT_SOURCE_DIR}/LockFreeSpscQueue
)

target_link_libraries(dragon_player_objs PRIVATE
    Qt6::Core
    Qt6::Network
    QCoro6::Core
)

set_target_properties(dragon_player_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(dragon_player_objs PRIVATE DragonMultimedia_EXPORTS)
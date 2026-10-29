add_executable(qfiledialogtest6 qfiledialogtest.cpp)
target_link_libraries(qfiledialogtest6 Qt6::Core Qt6::Widgets)

add_executable(systraymenuexclusiveactionstest6 systraymenuexclusiveactionstest.cpp)
target_link_libraries(systraymenuexclusiveactionstest6 Qt6::Core Qt6::Widgets)

if (QT_FEATURE_sessionmanager)
    add_executable(sessionrestorationtest6 sessionrestorationtest.cpp)
    target_link_libraries(sessionrestorationtest6 Qt6::Widgets Qt6::DBus)
endif()

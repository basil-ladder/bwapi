
include_directories(
  ../Util/Source
  ../BWAPI/openbw
  ../BWAPI/openbw/BWAPI
  ../BWAPI/Source/BWAPI
  ../BWAPI/Source
  ../BWAPICore
  ../Shared
)

add_executable(BWAPILauncher
  Source/Main.cpp
)

add_executable(ReplayViewer
  Source/ReplayViewer.cpp
)

target_link_libraries(BWAPILauncher BWAPI)
target_link_libraries(ReplayViewer BWAPI)

if (NOT WIN32)
  target_link_libraries(BWAPILauncher dl)
  target_link_libraries(ReplayViewer dl)
endif()

install(TARGETS BWAPILauncher ReplayViewer
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)

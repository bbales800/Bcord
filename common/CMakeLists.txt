add_library(BCordCommon
        src/dummy.cpp
)

# lets consumers include "lan_mac.hpp" directly
target_include_directories(BCordCommon PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

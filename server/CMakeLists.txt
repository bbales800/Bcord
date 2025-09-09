# server/CMakeLists.txt
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(redis++ CONFIG QUIET)   # ok if not found; we fall back
# pqxx (libpqxx-dev) is installed by apt in the Docker image; it provides target 'pqxx'

add_executable(BCordServer
        src/main.cpp
)

# Prevent libpqxx linking against std::source_location in our objects
target_compile_definitions(BCordServer PRIVATE PQXX_HIDE_SOURCE_LOCATION)

if (redis++_FOUND)
    target_link_libraries(BCordServer PRIVATE
            Boost::system
            Boost::thread
            pthread
            BCordCommon
            nlohmann_json::nlohmann_json
            redis++::redis++
            hiredis
            pqxx
            argon2
    )
else()
    message(WARNING "redis++ CONFIG not found; linking libs by name")
    target_link_libraries(BCordServer PRIVATE
            Boost::system
            Boost::thread
            pthread
            BCordCommon
            nlohmann_json::nlohmann_json
            redis++
            hiredis
            pqxx
            argon2
    )
endif()

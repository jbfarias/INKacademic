add_executable(ota_sector_writer_test OtaSectorWriterTest.cpp)
target_include_directories(ota_sector_writer_test PRIVATE stubs ${REPO_ROOT})
target_compile_options(ota_sector_writer_test PRIVATE -UNDEBUG -Wall -Wextra -pedantic)
add_test(NAME ota_sector_writer COMMAND ota_sector_writer_test)

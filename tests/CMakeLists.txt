add_executable(mvcc_hive_smoke smoke.cpp)
target_link_libraries(mvcc_hive_smoke PRIVATE mvcc_hive)

add_test(NAME mvcc_hive_smoke COMMAND mvcc_hive_smoke)

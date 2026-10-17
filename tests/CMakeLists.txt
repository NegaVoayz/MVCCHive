add_executable(mvcc_hive_layout_test layout_test.cpp)
target_link_libraries(mvcc_hive_layout_test PRIVATE mvcc_hive)

add_executable(mvcc_hive_storage_test storage_test.cpp)
target_link_libraries(mvcc_hive_storage_test PRIVATE mvcc_hive)

add_executable(mvcc_hive_history_test history_test.cpp)
target_link_libraries(mvcc_hive_history_test PRIVATE mvcc_hive)

add_executable(mvcc_hive_transaction_test transaction_test.cpp)
target_link_libraries(mvcc_hive_transaction_test PRIVATE mvcc_hive)

add_executable(mvcc_hive_prune_test prune_test.cpp)
find_package(Threads REQUIRED)
target_link_libraries(
    mvcc_hive_prune_test
    PRIVATE
        mvcc_hive
        Threads::Threads)

add_executable(mvcc_hive_model_test model_test.cpp)
target_link_libraries(mvcc_hive_model_test PRIVATE mvcc_hive)

add_executable(mvcc_hive_concurrency_test concurrency_test.cpp)
target_link_libraries(
    mvcc_hive_concurrency_test
    PRIVATE
        mvcc_hive
        Threads::Threads)

add_executable(mvcc_hive_lifetime_test lifetime_test.cpp)
target_link_libraries(mvcc_hive_lifetime_test PRIVATE mvcc_hive)

add_test(NAME mvcc_hive_layout COMMAND mvcc_hive_layout_test)
add_test(NAME mvcc_hive_storage COMMAND mvcc_hive_storage_test)
add_test(NAME mvcc_hive_history COMMAND mvcc_hive_history_test)
add_test(NAME mvcc_hive_transaction COMMAND mvcc_hive_transaction_test)
add_test(NAME mvcc_hive_prune COMMAND mvcc_hive_prune_test)
add_test(NAME mvcc_hive_model COMMAND mvcc_hive_model_test)
add_test(NAME mvcc_hive_concurrency COMMAND mvcc_hive_concurrency_test)
add_test(NAME mvcc_hive_lifetime COMMAND mvcc_hive_lifetime_test)

add_executable(
    mvcc_hive_commit_history_benchmark
    commit_history_benchmark.cpp)
target_link_libraries(
    mvcc_hive_commit_history_benchmark
    PRIVATE
        mvcc_hive)

add_executable(
    mvcc_hive_commit_log_storage_benchmark
    commit_log_storage_benchmark.cpp)
target_link_libraries(
    mvcc_hive_commit_log_storage_benchmark
    PRIVATE
        mvcc_hive)

#pragma once

#include <mvcc_hive/version_token.hpp>

#include <cstddef>

namespace mvcc_hive {

template<class T>
class MVCCHive {
public:
    using CommitId = detail::CommitId;
    using VirtualId = detail::VirtualId;
    using VersionToken = mvcc_hive::VersionToken;
    using Diff = mvcc_hive::Diff;
};

}  // namespace mvcc_hive

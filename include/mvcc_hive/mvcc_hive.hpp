#pragma once

// Public entry point for MVCCHive.
//
// The design is documented in docs/. Implementation starts with the layout,
// storage, and row-index milestones in docs/13-implementation-plan.md.

namespace mvcc_hive {

template<class T>
class MVCCHive;

}  // namespace mvcc_hive

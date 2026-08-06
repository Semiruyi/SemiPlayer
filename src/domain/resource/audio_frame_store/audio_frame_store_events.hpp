#pragma once

namespace semi::domain {

// Sent when the store changes from empty to non-empty.
struct AudioFrameStoreNotEmpty {};

// Sent when the store changes from full to non-full.
struct AudioFrameStoreNotFull {};

} // namespace semi::domain

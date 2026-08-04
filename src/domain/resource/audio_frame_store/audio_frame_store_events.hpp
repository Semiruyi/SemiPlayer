#pragma once

namespace semi::domain {

// Sent after an item is successfully pushed into the store.
struct AudioFrameStoreNotEmpty {};

// Sent when the store changes from full to non-full.
struct AudioFrameStoreNotFull {};

} // namespace semi::domain

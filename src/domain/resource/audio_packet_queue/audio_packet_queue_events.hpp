#pragma once

namespace semi::domain {

// Sent when the queue changes from empty to non-empty.
struct AudioQueueNotEmpty {};

// Sent when the queue changes from full to non-full.
struct AudioQueueNotFull {};

} // namespace semi::domain

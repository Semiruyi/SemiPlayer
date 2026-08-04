#pragma once

namespace semi::domain {

// Sent after an item is successfully pushed into the queue.
struct AudioQueueNotEmpty {};

// Sent when the queue changes from full to non-full.
struct AudioQueueNotFull {};

} // namespace semi::domain

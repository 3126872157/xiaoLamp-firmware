#include <cassert>
#include "boards/common/camera_vision_memory.h"
int main() {
    // Realistic residual internal heap after a conversation: the old 32K/8K
    // heuristic rejected this even though image + TLS storage use PSRAM.
    assert(camera_vision::CanStart(15000, 2800));
    assert(camera_vision::CanStart(8192, 1024));
    assert(!camera_vision::CanStart(8191, 4096));
    assert(!camera_vision::CanStart(64000, 1023));
    assert(camera_vision::Deferred("Vision deferred: internal_free=4096"));
    assert(!camera_vision::Deferred("Vision connection failed"));
    assert(!camera_vision::Deferred("Vision HTTP 401; verify service credentials/quota"));
    assert(!camera_vision::Deferred("JPEG PSRAM workspace allocation failed"));
}
